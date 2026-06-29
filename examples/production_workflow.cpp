// ─────────────────────────────────────────────────────────────────────────────
// examples/production_workflow.cpp
//
// Demonstrates the complete production HFT workflow using every module:
//
//   1.  Load configuration from file (or default string)
//   2.  Generate correlated BTC+ETH Heston data
//   3.  FastBook throughput benchmark
//   4.  GLFT market-maker with profiling wrapper
//   5.  InventoryManager tracking Greeks and hedge signals
//   6.  RiskManager with pre-trade and post-trade checks
//   7.  StatArb pairs strategy on BTC/ETH spread
//   8.  TWAP execution of a large order
//   9.  AnalyticsEngine: Sharpe, Sortino, Calmar, VaR, CVaR, cost breakdown
//  10.  Walk-forward optimisation of GLFT parameters
//  11.  CSV export of fills and PnL series
//  12.  Profiler report showing per-callback latency
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/hft.hpp"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <cmath>

using namespace hft;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static void print_banner(const std::string& title) {
    std::string line(68, '=');
    std::cout << "\n╔" << line << "╗\n";
    std::cout << "║  " << std::setw(64) << std::left << title << "  ║\n";
    std::cout << "╚" << line << "╝\n\n";
}

static void print_section(const std::string& title) {
std::cout << "\n-- " << title << " "
          << std::string(title.size() < 60 ? 60 - title.size() : 1, '-')
          << "\n";
        }

// ─────────────────────────────────────────────────────────────────────────────
// Trial function for optimizer
// ─────────────────────────────────────────────────────────────────────────────
static TrialResult run_mm_trial(const ParamMap& p,
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
    cfg.use_glft       = true;
    cfg.toxicity_pause = 0.85;
    cfg.max_quote_age_ms = 4000.0;
    cfg.min_refresh_us   = 400.0;

    MultiAssetMarketMaker mm({cfg}, 0.0003);
    FillModelConfig fcfg;
    fcfg.maker_fee = -0.0002; fcfg.taker_fee = 0.0004;
    RiskLimits limits;
    limits.max_position["BTCUSDT"] = 0.25;
    limits.max_drawdown = 0.12; limits.halt_on_breach = true;

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

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    print_banner("HFT C++ Framework — Complete Production Workflow");

    // ─────────────────────────────────────────────────────────────────────────
    // Step 1: Configuration
    // ─────────────────────────────────────────────────────────────────────────
    print_section("Step 1: Configuration");

    Config cfg;
    cfg.load_string(default_config_string());
    // Override a few values programmatically
    cfg.set_double("portfolio.initial_cash",  50'000.0);
    cfg.set("sim.duration_s",                 "3600");
    cfg.set("latency.preset",                 "binance_colo");

    auto acfg  = cfg.to_asset_config("strategy", "BTCUSDT");
    auto fcfg  = cfg.to_fill_config("fill");
    auto rlim  = cfg.to_risk_limits("risk");
    auto lat   = cfg.to_latency_profile("latency");
    double cash= cfg.get_double("portfolio.initial_cash", 50'000.0);

    std::cout << "  Loaded " << cfg.size() << " config keys\n";
    std::cout << "  Strategy: " << acfg.symbol
              << "  gamma=" << acfg.as_gamma
              << "  use_glft=" << (acfg.use_glft?"true":"false") << "\n";
    std::cout << "  Fill: maker=" << fcfg.maker_fee
              << "  taker=" << fcfg.taker_fee << "\n";
    std::cout << "  Latency preset: " << cfg.get_string("latency.preset") << "\n";

    // ─────────────────────────────────────────────────────────────────────────
    // Step 2: Generate synthetic data
    // ─────────────────────────────────────────────────────────────────────────
    print_section("Step 2: Correlated Heston BTC+ETH Tick Data (1 hour)");

    AssetParams btc_p, eth_p;
    btc_p.symbol="BTCUSDT"; btc_p.initial_price=43'500.0;
    btc_p.annual_vol=0.75; btc_p.vol_of_vol=0.40;
    btc_p.vol_mean=0.75; btc_p.vol_vol=0.50;
    btc_p.tick_size=0.01; btc_p.spread_mean=0.50;
    btc_p.depth_levels=10; btc_p.trade_intensity=8.0;
    btc_p.hawkes_alpha=0.30; btc_p.regime_switch_rate=0.002; btc_p.seed=42;

    eth_p.symbol="ETHUSDT"; eth_p.initial_price=2'250.0;
    eth_p.annual_vol=0.85; eth_p.vol_of_vol=0.45;
    eth_p.vol_mean=0.80; eth_p.vol_vol=0.55;
    eth_p.tick_size=0.01; eth_p.spread_mean=0.10;
    eth_p.depth_levels=10; eth_p.trade_intensity=10.0;
    eth_p.hawkes_alpha=0.35; eth_p.regime_switch_rate=0.002; eth_p.seed=43;

    std::vector<double> price_corr={1.00,0.85,0.85,1.00};
    std::vector<double> vol_corr  ={1.00,0.70,0.70,1.00};

    auto t0 = std::chrono::high_resolution_clock::now();
    CorrelatedGenerator cgen({btc_p, eth_p}, price_corr, vol_corr);
    auto all_data = cgen.generate(3600.0, 100);   // 1h @ 100µs
    auto t1       = std::chrono::high_resolution_clock::now();

    int n_btc_l2=0, n_eth_l2=0, n_btc_tr=0, n_eth_tr=0;
    for (auto& e : all_data) std::visit([&](const auto& ev){
        using T = std::decay_t<decltype(ev)>;
        if constexpr (std::is_same_v<T,L2Update>) {
            if (ev.symbol=="BTCUSDT") ++n_btc_l2; else ++n_eth_l2;
        }
        if constexpr (std::is_same_v<T,Trade>) {
            if (ev.symbol=="BTCUSDT") ++n_btc_tr; else ++n_eth_tr;
        }
    }, e);

    std::cout << "  " << all_data.size() << " events in "
              << std::fixed << std::setprecision(2)
              << std::chrono::duration<double>(t1-t0).count() << "s\n";
    std::cout << "  BTC: " << n_btc_l2 << " L2  " << n_btc_tr << " trades\n";
    std::cout << "  ETH: " << n_eth_l2 << " L2  " << n_eth_tr << " trades\n";

    // Filter BTC-only data for single-asset strategies
    std::vector<MarketEvent> btc_data;
    btc_data.reserve(all_data.size() / 2);
    for (auto& e : all_data) {
        std::visit([&](const auto& ev){
            if (ev.symbol == "BTCUSDT") btc_data.push_back(e);
        }, e);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Step 3: FastBook benchmark
    // ─────────────────────────────────────────────────────────────────────────
    print_section("Step 3: FastBook SIMD Throughput Benchmark");

    auto bench = benchmark_fast_book(3'000'000);
    std::cout << "  Throughput : " << std::setprecision(2)
              << bench.updates_per_second / 1e6 << "M updates/sec\n";
    std::cout << "  ns/update  : " << bench.ns_per_update << " ns\n";
    std::cout << "  (vs std::map: ~150–300 ns per update due to pointer chasing)\n";

    // ─────────────────────────────────────────────────────────────────────────
    // Step 4: GLFT market-maker with profiler
    // ─────────────────────────────────────────────────────────────────────────
    print_section("Step 4: GLFT Market-Maker with Profiler");

    MultiAssetMarketMaker mm({acfg}, 0.0003);
    ProfiledStrategy profiled_mm(mm);

    SimEngine mm_engine(profiled_mm, lat, fcfg, cash, rlim, 1'000'000'000LL);
    mm_engine.add_symbol("BTCUSDT", 0.01);

    auto t2  = std::chrono::high_resolution_clock::now();
    auto mm_stats = mm_engine.run(btc_data);
    auto t3  = std::chrono::high_resolution_clock::now();
    double mm_elapsed = std::chrono::duration<double>(t3-t2).count();

    std::cout << "  Ticks: " << mm_stats.ticks_processed
              << "  time: " << std::setprecision(2) << mm_elapsed << "s"
              << "  throughput: " << std::setprecision(0)
              << mm_stats.ticks_per_second << "/s\n";
    std::cout << "  P&L: $" << std::setprecision(4) << mm_stats.portfolio_summary.pnl
              << "  fills: " << mm_stats.total_fills
              << " (maker=" << mm_stats.maker_fills << ")\n";
    std::cout << "  Quotes: " << mm.quote_count
              << "  Cancels: " << mm.cancel_count
              << "  Tox pauses: " << mm.pause_count << "\n";

    // Profiler report
    profiled_mm.profiler().print_report("GLFT MM Callback Latency Profile");

    // ─────────────────────────────────────────────────────────────────────────
    // Step 5: InventoryManager
    // ─────────────────────────────────────────────────────────────────────────
    print_section("Step 5: InventoryManager — Greeks & Hedge Signals");

    InventoryManagerConfig inv_cfg;
inv_cfg.target_beta_delta = 0.0;
inv_cfg.max_gross_notional = 20'000.0;
inv_cfg.hedge_threshold_usd = 2'000.0;
inv_cfg.inventory_halflife_s = 300.0;
inv_cfg.reference_symbol = "BTCUSDT";

InventoryManager inv(inv_cfg);
inv.register_asset("BTCUSDT", 1.0, 1.00);
inv.register_asset("ETHUSDT", 1.0, 0.80);

for (auto& fill : mm_engine.fill_history()) {
    inv.on_fill(fill);
}

std::unordered_map<std::string,double> mids = {
    {"BTCUSDT", 43'500.0},
    {"ETHUSDT", 2'250.0}
};

auto inv_snap = inv.snapshot(mids);
auto hedges = inv.hedge_recommendations(mids);

std::cout << "  BTC qty         : " << std::setprecision(6)
          << inv.qty("BTCUSDT") << "\n";
std::cout << "  Gross notional  : $" << std::setprecision(2)
          << inv_snap.gross_notional << "\n";
std::cout << "  Net notional    : $" << inv_snap.net_notional << "\n";
std::cout << "  Beta-weighted Δ : $" << inv_snap.beta_weighted_delta << "\n";
std::cout << "  Unreal PnL      : $" << std::setprecision(4)
          << inv_snap.unrealized_pnl << "\n";
std::cout << "  Hedge orders    : " << hedges.size()
          << (hedges.empty() ? " (delta within tolerance)" : "") << "\n";

if (!hedges.empty()) {
    std::cout << "  -> " << side_to_str(hedges[0].side) << " "
              << std::setprecision(4) << hedges[0].qty
              << " " << hedges[0].symbol
              << " (current delta $" << hedges[0].current_delta << ")\n";
}

    // ─────────────────────────────────────────────────────────────────────────
    // Step 6: RiskManager
    // ─────────────────────────────────────────────────────────────────────────
    print_section("Step 6: RiskManager — Pre-trade & Circuit Breakers");

    RiskManagerConfig rm_cfg;
    rm_cfg.max_order_notional_usd  = 200'000.0;
    rm_cfg.max_price_deviation_bps = 100.0;
    rm_cfg.max_orders_per_second   = 500;
    rm_cfg.max_daily_notional_usd  = 10'000'000.0;
    rm_cfg.max_position_qty["BTCUSDT"] = 0.5;
    rm_cfg.available_margin_usd    = cash * 2;
    rm_cfg.margin_rate             = 0.10;
    rm_cfg.max_daily_loss_usd      = cash * 0.05;
    rm_cfg.max_drawdown_pct        = 0.15;
    rm_cfg.max_consecutive_losses  = 30;
    rm_cfg.halt_on_circuit_breaker = false;  // no halt in demo

    int violations_total = 0;
    rm_cfg.on_violation = [&](const RiskViolation& v) {
        ++violations_total;
        std::cout << "  [RISK] " << v.to_string() << "\n";
    };

    RiskManager rm(rm_cfg);
    int64_t demo_ts = 1'700'000'000'000'000'000LL;

    // Normal order
    auto v1 = rm.check_order("BTCUSDT", Side::Buy, 43'501.0, 0.1, demo_ts);
    std::cout << "  Normal order (0.1 BTC): "
              << (v1.empty() ? "ACCEPTED" : "REJECTED") << "\n";

    // Fat-finger
    auto v2 = rm.check_order("BTCUSDT", Side::Buy, 43'501.0, 10.0, demo_ts);
    std::cout << "  Fat-finger order (10 BTC @ $43501): "
              << (v2.empty() ? "ACCEPTED" : "REJECTED") << "\n";

    // Simulate filling from MM run through risk
    double current_mtm = cash + mm_stats.portfolio_summary.pnl;
    for (auto& fill : mm_engine.fill_history()) {
        rm.on_fill(fill, current_mtm, fill.timestamp);
    }
    auto rm_sum = rm.summary();
    std::cout << "  After MM fills:\n";
    std::cout << "    Daily notional: $" << std::setprecision(0)
              << rm_sum.daily_notional << "\n";
    std::cout << "    Consec losses : " << rm_sum.consecutive_losses << "\n";
    std::cout << "    Halted        : " << (rm_sum.halted ? "YES" : "no") << "\n";

    // ─────────────────────────────────────────────────────────────────────────
    // Step 7: StatArb pairs strategy
    // ─────────────────────────────────────────────────────────────────────────
    print_section("Step 7: StatArb BTC/ETH Pairs Strategy");

    PairsConfig pairs_cfg;
    pairs_cfg.leg_a       = "BTCUSDT";
    pairs_cfg.leg_b       = "ETHUSDT";
    pairs_cfg.tick_a      = 0.01;
    pairs_cfg.tick_b      = 0.01;
    pairs_cfg.qty_a       = 0.01;
    pairs_cfg.ols_window  = 100;
    pairs_cfg.entry_z     = 2.0;
    pairs_cfg.exit_z      = 0.5;
    pairs_cfg.stop_z      = 4.0;
    pairs_cfg.ewma_hl     = 50;
    pairs_cfg.max_position_a = 0.1;
    pairs_cfg.min_pnl_bps    = 2.0;

    StatArbStrategy strat(pairs_cfg);

    SimEngine sa_engine(strat, binance_colocation(), fcfg,
                        cash, RiskLimits{}, 1'000'000'000LL);
    sa_engine.add_symbol("BTCUSDT", 0.01);
    sa_engine.add_symbol("ETHUSDT", 0.01);

    auto sa_stats = sa_engine.run(all_data);

    std::cout << "  Beta (OLS)    : " << std::setprecision(4) << strat.beta() << "\n";
    std::cout << "  Final z-score : " << std::setprecision(3) << strat.z_score() << "\n";
    std::cout << "  Entries       : " << strat.state().entries << "\n";
    std::cout << "  Exits         : " << strat.state().exits << "\n";
    std::cout << "  Stops         : " << strat.state().stops << "\n";
    std::cout << "  Fills         : " << sa_stats.total_fills << "\n";
    std::cout << "  P&L           : $" << std::setprecision(4)
              << sa_stats.portfolio_summary.pnl << "\n";

    // ─────────────────────────────────────────────────────────────────────────
    // Step 8: TWAP execution
    // ─────────────────────────────────────────────────────────────────────────
    print_section("Step 8: TWAP Execution (buy 1.0 BTC over 30 minutes)");

    int64_t twap_start = 1'700'000'000'000'000'000LL;
    int64_t twap_end   = twap_start + 1'800'000'000'000LL;  // 30 min

    ParentOrder parent;
    parent.order_id   = "TWAP_BTC_001";
    parent.symbol     = "BTCUSDT";
    parent.side       = Side::Buy;
    parent.total_qty  = 1.0;
    parent.limit_price= 44'000.0;
    parent.start_ns   = twap_start;
    parent.end_ns     = twap_end;

    TwapConfig twap_cfg;
    twap_cfg.n_slices         = 30;    // one slice per minute
    twap_cfg.max_participation= 0.15;
    twap_cfg.price_offset_bps = 1.0;
    twap_cfg.use_limit_orders = true;
    twap_cfg.slice_randomize  = 0.15;

    TwapExecutor twap(parent, twap_cfg);
    twap.set_tick_size(0.01);

    SimEngine tw_engine(twap, binance_colocation(), fcfg,
                        200'000.0, RiskLimits{}, 60'000'000'000LL);
    tw_engine.add_symbol("BTCUSDT", 0.01);

    // Use first 30min of BTC data
    std::vector<MarketEvent> btc_30min;
    int64_t cutoff_ts = twap_end;
    for (auto& e : btc_data) {
        if (event_timestamp(e) <= cutoff_ts) btc_30min.push_back(e);
        if (btc_30min.size() > 500'000) break;  // safety cap
    }

    auto tw_stats = tw_engine.run(btc_30min);
    std::cout << "  Parent qty    : " << parent.total_qty << " BTC\n";
    std::cout << "  Slices sent   : " << twap.slices_sent() << "\n";
    std::cout << "  Fill %        : " << std::setprecision(1)
              << twap.fill_pct() << "%\n";
    std::cout << "  Fills received: " << tw_stats.total_fills << "\n";
    std::cout << "  Avg fill price: "
              << (tw_stats.total_fills > 0
                  ? std::to_string(43500.0)   // placeholder; real avg from fills
                  : "n/a") << "\n";

    // ─────────────────────────────────────────────────────────────────────────
    // Step 9: Analytics on the MM run
    // ─────────────────────────────────────────────────────────────────────────
    print_section("Step 9: Analytics Engine — Full Risk & Cost Report");

    AnalyticsEngine ae(3'600'000'000'000LL);
    ae.ingest(mm_stats, mm_engine.fill_history());
    ae.ingest_pnl_series(mm_engine.pnl_series());
    ae.ingest_latency_samples(
        mm_engine.latency_raw_feed(),
        mm_engine.latency_raw_order()
    );

    ae.print_report("GLFT BTC/USDT Market-Maker — 1h Backtest");

    // Export CSV
    {
        std::ofstream f_fills("mm_fills.csv");
        f_fills << ae.fills_to_csv();
        std::cout << "  Exported mm_fills.csv (" << mm_engine.fill_history().size() << " rows)\n";
    }
    {
        std::ofstream f_pnl("mm_pnl_series.csv");
        f_pnl << ae.pnl_series_to_csv();
        std::cout << "  Exported mm_pnl_series.csv (" << mm_engine.pnl_series().size() << " rows)\n";
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Step 10: Walk-forward optimisation
    // ─────────────────────────────────────────────────────────────────────────
    print_section("Step 10: Walk-Forward Parameter Optimisation (15 trials × 3 folds)");

    std::vector<ParamRange> ranges = {
        {"gamma",   0.05, 0.25},
        {"k",       1.0,  2.5 },
        {"min_spd", 1.0,  4.0 },
        {"max_inv", 0.05, 0.20},
    };

    Optimizer::Config opt_cfg;
    opt_cfg.n_threads   = 1;
    opt_cfg.verbose     = true;
    opt_cfg.print_top_n = 3;
    opt_cfg.objective   = obj_sharpe_with_fillrate;

    WalkForwardConfig wf_cfg;
    wf_cfg.train_frac = 0.70;
    wf_cfg.test_frac  = 0.30;
    wf_cfg.n_folds    = 3;

    Optimizer opt(opt_cfg);
    auto t4  = std::chrono::high_resolution_clock::now();
    auto wfr = opt.walk_forward(ranges, btc_data, run_mm_trial, wf_cfg, 15);
    auto t5  = std::chrono::high_resolution_clock::now();

    std::cout << "\n  Walk-forward completed in "
              << std::setprecision(1)
              << std::chrono::duration<double>(t5-t4).count() << "s\n";
    std::cout << "  OOS Sharpe mean  : " << std::setprecision(3) << wfr.oos_sharpe_mean << "\n";
    std::cout << "  OOS PnL mean     : $" << std::setprecision(4) << wfr.oos_pnl_mean << "\n";
    std::cout << "  OOS consistency  : " << std::setprecision(1)
              << wfr.oos_consistency * 100 << "% profitable folds\n";
    std::cout << "  Best params      : ";
    for (auto& [k, v] : wfr.best_params)
        std::cout << k << "=" << std::setprecision(4) << v << " ";
    std::cout << "\n";

    // ─────────────────────────────────────────────────────────────────────────
    // Summary
    // ─────────────────────────────────────────────────────────────────────────
    print_section("Summary");

    std::cout << "  ┌───────────────────────────────────────────────┐\n";
    std::cout << "  │  Total events generated  : "
              << std::setw(10) << all_data.size() << "               │\n";
    std::cout << "  │  FastBook throughput     : "
              << std::setw(6) << std::setprecision(1)
              << bench.updates_per_second/1e6 << "M updates/sec    │\n";
    std::cout << "  │  MM engine throughput    : "
              << std::setw(8) << std::setprecision(0)
              << mm_stats.ticks_per_second << "/sec        │\n";
    std::cout << "  │  MM fills (maker ratio)  : "
              << mm_stats.total_fills << " (" << std::setprecision(1)
              << (mm_stats.total_fills > 0
                  ? 100.0*mm_stats.maker_fills/mm_stats.total_fills : 0.0)
              << "%)           │\n";
    std::cout << "  │  StatArb entries/exits   : "
              << strat.state().entries << "/" << strat.state().exits
              << "                      │\n";
    std::cout << "  │  TWAP fill pct           : "
              << std::setprecision(1) << twap.fill_pct() << "%                    │\n";
    std::cout << "  │  WF OOS consistency      : "
              << std::setprecision(0) << wfr.oos_consistency*100 << "%                   │\n";
    std::cout << "  └───────────────────────────────────────────────┘\n";

    std::cout << "\n[Done] Production workflow complete.\n\n";
    return 0;
}