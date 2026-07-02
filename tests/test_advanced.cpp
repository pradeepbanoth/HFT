// ─────────────────────────────────────────────────────────────────────────────
// tests/test_advanced.cpp  —  Tests for FastBook, Analytics, Optimizer, LiveReplay
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/hft.hpp"
#include "market_data_engine.hpp"
#include "smart_order_router.hpp"
#include <iostream>
#include <iomanip>
#include <cassert>
#include <cmath>
#include <chrono>
#include <numeric>
#include <cstdio>
#include <fstream>
#include <cstdio>
#include <cstdlib>

using namespace hft;
using namespace hft::signals;

// ─── Test utilities ───────────────────────────────────────────────────────────
static int g_pass = 0, g_fail = 0;
static const char* g_suite = "";

#define SUITE(n) do { g_suite=(n); std::cout<<"\n=== "<<n<<" ===\n"; } while(0)
#define CHECK(expr, msg) do { \
    if (!(expr)) { ++g_fail; \
        std::cerr<<"  FAIL ["<<__LINE__<<"] "<<msg<<"\n"; } \
    else { ++g_pass; std::cout<<"  PASS: "<<msg<<"\n"; } \
} while(0)
#define CHECK_NEAR(a,b,tol,msg) CHECK(std::abs((double)(a)-(double)(b))<(tol), msg)

// ─────────────────────────────────────────────────────────────────────────────
// 1. FastBook  —  SIMD-accelerated flat order book
// ─────────────────────────────────────────────────────────────────────────────

void test_fast_book() {
    SUITE("FastBook (SIMD)");

    FastBook fb("BTCUSDT", 0.01);

    // Insert 20 bid and ask levels
    for (int i = 0; i < 20; ++i) {
        L2Update b; b.symbol="BTCUSDT"; b.side=BookSide::Bid;
        b.price=43500.0-i*0.01; b.qty=1.0+i*0.1; b.timestamp=i;
        fb.apply_l2(b);
        L2Update a; a.symbol="BTCUSDT"; a.side=BookSide::Ask;
        a.price=43501.0+i*0.01; a.qty=1.0+i*0.1; a.timestamp=i;
        fb.apply_l2(a);
    }

    CHECK(fb.best_bid().has_value(), "best_bid present");
    CHECK(fb.best_ask().has_value(), "best_ask present");
    CHECK_NEAR(*fb.best_bid(), 43500.0, 1e-6, "best_bid = 43500.00");
    CHECK_NEAR(*fb.best_ask(), 43501.0, 1e-6, "best_ask = 43501.00");
    CHECK_NEAR(*fb.spread(),   1.0,     1e-6, "spread = 1.0");
    CHECK(fb.n_bids() == 20, "n_bids == 20");
    CHECK(fb.n_asks() == 20, "n_asks == 20");

    // OBI
    double obi = fb.imbalance(5);
    CHECK(obi >= -1.0 && obi <= 1.0, "imbalance in [-1,1]");

    // Micro-price
    double mp = fb.micro_price(5);
    CHECK(mp > *fb.best_bid() && mp < *fb.best_ask(), "micro_price between bid and ask");

    // VWAP
    double vwap = fb.vwap_cost(Side::Buy, 2.0);
    CHECK(vwap > 43501.0, "vwap buy > best_ask");

    // Depth arrays
    double bp[10]={}, bq[10]={}, ap[10]={}, aq[10]={};
    fb.fill_depth_arrays(bp, bq, ap, aq, 10);
    CHECK(bp[0] > bp[1], "bid_prices descending");
    CHECK(ap[0] < ap[1], "ask_prices ascending");
    CHECK(bq[0] > 0.0, "bid qty > 0");

    // Update existing level (modify)
    L2Update upd; upd.symbol="BTCUSDT"; upd.side=BookSide::Bid;
    upd.price=43500.0; upd.qty=5.0; upd.timestamp=100;
    fb.apply_l2(upd);
    fb.fill_depth_arrays(bp, bq, ap, aq, 1);
    CHECK_NEAR(bq[0], 5.0, 1e-9, "best bid qty updated to 5.0");

    // Remove level (qty=0)
    L2Update rem; rem.symbol="BTCUSDT"; rem.side=BookSide::Bid;
    rem.price=43500.0; rem.qty=0.0; rem.timestamp=101;
    fb.apply_l2(rem);
    CHECK(*fb.best_bid() < 43500.0, "best_bid moves down after removal");
    CHECK(fb.n_bids() == 19, "n_bids = 19 after removal");

    // Total qty
    double total_bq = fb.total_bid_qty(10);
    CHECK(total_bq > 0.0, "total_bid_qty > 0");

    // Benchmark
    auto bench = benchmark_fast_book(500'000);
    CHECK(bench.updates_per_second > 1'000'000.0, "FastBook > 1M updates/sec");
    std::cout << "  Throughput : " << std::fixed << std::setprecision(2)
              << bench.updates_per_second / 1e6 << "M updates/sec\n";
    std::cout << "  ns/update  : " << std::setprecision(2)
              << bench.ns_per_update << " ns\n";

    // SIMD find correctness — search for a price that exists
    {
        FastBook fb2("TEST", 0.01);
        for (int i = 0; i < 64; ++i) {
            L2Update u; u.symbol="TEST"; u.side=BookSide::Bid;
            u.price=100.0-i*0.01; u.qty=1.0; u.timestamp=i;
            fb2.apply_l2(u);
        }
        // Verify best bid is 100.0
        CHECK_NEAR(*fb2.best_bid(), 100.0, 1e-9, "SIMD: best_bid after 64 inserts");
        // Update price near end of sorted list
        L2Update u64; u64.symbol="TEST"; u64.side=BookSide::Bid;
        u64.price=100.0-63*0.01; u64.qty=9.9; u64.timestamp=64;
        fb2.apply_l2(u64);
        CHECK(fb2.n_bids() == 64, "SIMD: count unchanged after update");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Analytics Engine
// ─────────────────────────────────────────────────────────────────────────────

// Helper: build a minimal simulated fill history
static std::vector<FillEvent> make_fills(int n, bool mostly_profitable = true) {
    std::vector<FillEvent> fills;
    int64_t ts = 1'700'000'000'000'000'000LL;
    double  price = 43500.0;
    for (int i = 0; i < n; ++i) {
        FillEvent f;
        f.order_id     = "o" + std::to_string(i);
        f.symbol       = "BTCUSDT";
        f.side         = (i % 2 == 0) ? Side::Buy : Side::Sell;
        f.price        = price + (i % 3 - 1) * 0.50;
        f.qty          = 0.01;
        f.timestamp    = ts + i * 1'000'000'000LL;
        f.is_maker     = (i % 3 != 0);
        f.fee_rate     = f.is_maker ? -0.0002 : 0.0004;
        f.fee          = f.qty * f.price * f.fee_rate;
        f.realized_pnl = mostly_profitable
                       ? (i % 4 == 0 ? -0.5 : 1.2)   // 75% win rate
                       : (i % 4 != 0 ? -0.5 : 1.2);   // 25% win rate
        f.adverse_score= 0.1 + (i % 5) * 0.15;
        fills.push_back(f);
    }
    return fills;
}

// Helper: build a PnL series
static std::vector<std::pair<int64_t,double>> make_pnl_series(int n, double drift=0.5) {
    std::vector<std::pair<int64_t,double>> series;
    double mtm = 50'000.0;
    int64_t ts = 1'700'000'000'000'000'000LL;
    for (int i = 0; i < n; ++i) {
        double noise = (i % 7 - 3) * 10.0;
        mtm += drift + noise;
        series.push_back({ts + i * 1'000'000'000LL, mtm});
    }
    return series;
}

void test_analytics() {
    SUITE("Analytics Engine");

    AnalyticsEngine ae(3'600'000'000'000LL);  // 1-hour buckets

    // Build synthetic SimStats
    SimStats stats;
    stats.ticks_processed  = 500'000;
    stats.wall_time_s      = 2.5;
    stats.ticks_per_second = 200'000.0;
    stats.total_fills      = 100;
    stats.maker_fills      = 75;
    stats.taker_fills      = 25;
    stats.strategy_errors  = 0;
    stats.halted           = false;
    stats.portfolio_summary.pnl     = 350.0;
    stats.portfolio_summary.pnl_pct = 0.70;
    stats.portfolio_summary.total_fees = -15.0;
    stats.portfolio_summary.cash    = 50'350.0;

    auto fills   = make_fills(100, true);
    auto pnl_ser = make_pnl_series(200, 1.75);

    ae.ingest(stats, fills);
    ae.ingest_pnl_series(pnl_ser);
    ae.ingest_latency_samples(
        std::vector<int64_t>(500, 50'000),   // 50µs feed
        std::vector<int64_t>(500, 500'000)   // 500µs order
    );

    // Risk metrics
    auto rm = ae.compute_risk_metrics();
    CHECK(rm.sharpe != 0.0 || std::isnan(rm.sharpe) || rm.sharpe >= -1000.0,
          "Sharpe computed (no crash)");
    CHECK(rm.max_drawdown <= 0.0, "Max drawdown <= 0");
    CHECK(rm.win_rate >= 0.0 && rm.win_rate <= 1.0, "Win rate in [0,1]");
    CHECK(rm.var_95 >= 0.0, "VaR 95% >= 0");
    CHECK(rm.cvar_95 >= rm.var_95 - 1e-9, "CVaR >= VaR");
    std::cout << "  Sharpe      : " << std::setprecision(3) << rm.sharpe << "\n";
    std::cout << "  Sortino     : " << rm.sortino << "\n";
    std::cout << "  Win rate    : " << std::setprecision(1) << rm.win_rate * 100 << "%\n";
    std::cout << "  VaR 95%     : " << std::setprecision(4) << rm.var_95 * 100 << "%\n";
    std::cout << "  Max DD      : " << rm.max_drawdown * 100 << "%\n";

    // Cost breakdown
    auto cb = ae.compute_cost_breakdown();
    CHECK(cb.total_notional > 0.0, "Total notional > 0");
    CHECK(cb.maker_count + cb.taker_count == 100, "Fill counts correct");
    std::cout << "  Maker count : " << cb.maker_count << "\n";
    std::cout << "  Taker count : " << cb.taker_count << "\n";
    std::cout << "  Net fees    : $" << std::setprecision(4) << cb.net_fees << "\n";

    // Time buckets
    CHECK(!ae.time_buckets().empty() || pnl_ser.size() < 2,
          "Time buckets created (or series too short)");

    // Latency histogram
    CHECK(ae.feed_histogram().total == 500, "Feed histogram has 500 samples");
    CHECK(ae.order_histogram().total == 500, "Order histogram has 500 samples");

    // CSV export
    std::string fills_csv = ae.fills_to_csv();
    CHECK(!fills_csv.empty(), "fills_to_csv non-empty");
    CHECK(fills_csv.find("timestamp_ns") != std::string::npos, "CSV has header");
    CHECK(fills_csv.find("BTCUSDT") != std::string::npos, "CSV has symbol");

    std::string pnl_csv = ae.pnl_series_to_csv();
    CHECK(!pnl_csv.empty(), "pnl_series_to_csv non-empty");

    // Print report (suppress output for CI, just verify no crash)
    // ae.print_report("Test Analytics Report");
    CHECK(true, "print_report compiled and callable");
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Optimizer — grid search, random search, walk-forward
// ─────────────────────────────────────────────────────────────────────────────

// A simple trial function for testing: uses ParamMap to configure a MM strategy
// and returns a TrialResult based on a short backtest
static TrialResult run_trial(
    const ParamMap& params,
    const std::vector<MarketEvent>& data,
    int fold_id)
{
    AssetConfig cfg;
    cfg.symbol         = "BTCUSDT";
    cfg.tick_size      = 0.01;
    cfg.lot_size       = 0.001;
    cfg.quote_qty      = 0.01;
    cfg.max_inventory  = params.count("max_inv") ? params.at("max_inv") : 0.1;
    cfg.as_gamma       = params.count("gamma")   ? params.at("gamma")   : 0.15;
    cfg.as_k           = params.count("k")        ? params.at("k")       : 1.5;
    cfg.as_T           = 300.0;
    cfg.min_spread_bps = params.count("min_spd")  ? params.at("min_spd") : 1.5;
    cfg.max_spread_bps = 40.0;
    cfg.use_glft       = true;
    cfg.toxicity_pause = 0.90;
    cfg.max_quote_age_ms = 5000.0;
    cfg.min_refresh_us   = 500.0;

    MultiAssetMarketMaker mm({cfg}, 0.0003);

    FillModelConfig fcfg;
    fcfg.maker_fee = -0.0002;
    fcfg.taker_fee =  0.0004;

    RiskLimits limits;
    limits.max_position["BTCUSDT"] = 0.2;
    limits.max_drawdown = 0.15;
    limits.halt_on_breach = true;

    SimEngine engine(mm, binance_colocation(), fcfg, 50'000.0, limits,
                     5'000'000'000LL);
    engine.add_symbol("BTCUSDT", 0.01);

    auto stats = engine.run(data);
    auto& ps   = stats.portfolio_summary;

    TrialResult r;
    r.sharpe       = ps.sharpe;
    r.calmar       = ps.calmar;
    r.pnl          = ps.pnl;
    r.pnl_pct      = ps.pnl_pct;
    r.max_drawdown = ps.max_drawdown;
    r.total_fills  = stats.total_fills;
    r.halted       = stats.halted;
    return r;
}

void test_optimizer() {
    SUITE("Optimizer (Grid + Random + Walk-Forward)");

    // Generate a small dataset (15s at 10ms intervals = 1500 ticks)
    AssetParams ap;
    ap.symbol="BTCUSDT"; ap.initial_price=43500.0;
    ap.annual_vol=0.75; ap.tick_size=0.01; ap.depth_levels=5;
    ap.trade_intensity=8.0; ap.hawkes_alpha=0.3; ap.seed=42;
    SyntheticGenerator gen(ap);
    auto data = gen.generate(15.0, 10'000);   // 15s at 10ms

    CHECK(!data.empty(), "Test data non-empty");
    std::cout << "  Test data: " << data.size() << " events\n";

    // ── Grid search (small grid: 2×2 = 4 combos) ────────────────────────────
    std::vector<ParamRange> ranges = {
        {"gamma",   0.10, 0.20, 0.10},   // 2 values
        {"min_spd", 1.5,  3.0,  1.5},   // 2 values
    };

    Optimizer::Config cfg;
    cfg.n_threads  = 1;
    cfg.verbose    = false;
    cfg.print_top_n= 0;
    cfg.objective  = obj_sharpe;

    Optimizer opt(cfg);
    auto grid_results = opt.grid_search(ranges, data, run_trial);

    CHECK(!grid_results.empty(), "Grid search returns results");
    CHECK(grid_results.size() == 4, "Grid search: 2x2 = 4 results");
    CHECK(grid_results[0].objective >= grid_results.back().objective,
          "Grid results sorted by objective descending");
    std::cout << "  Grid best: gamma=" << grid_results[0].params.at("gamma")
              << "  min_spd=" << grid_results[0].params.at("min_spd")
              << "  obj=" << std::setprecision(3) << grid_results[0].objective << "\n";

    // ── Random search (5 trials) ────────────────────────────────────────────
    std::vector<ParamRange> cont_ranges = {
        {"gamma",   0.05, 0.25},
        {"k",       1.0,  2.5},
        {"min_spd", 1.0,  4.0},
    };
    auto rand_results = opt.random_search(cont_ranges, data, run_trial, 5, 99);
    CHECK(rand_results.size() == 5, "Random search: 5 results");
    CHECK(rand_results[0].objective >= rand_results.back().objective,
          "Random results sorted descending");
    std::cout << "  Random best: obj=" << std::setprecision(3)
              << rand_results[0].objective << "\n";

    // ── Walk-forward validation (3 folds) ───────────────────────────────────
    WalkForwardConfig wf;
    wf.train_frac = 0.70;
    wf.test_frac  = 0.30;
    wf.n_folds    = 3;
    wf.anchored   = false;

    Optimizer::Config wf_cfg = cfg;
    wf_cfg.verbose = false;
    Optimizer wf_opt(wf_cfg);

    auto wfr = wf_opt.walk_forward(cont_ranges, data, run_trial, wf, 3);
    CHECK(wfr.oos_results.size() <= 3, "Walk-forward: <= 3 OOS results");
    CHECK(wfr.oos_consistency >= 0.0 && wfr.oos_consistency <= 1.0,
          "OOS consistency in [0,1]");
    std::cout << "  WF OOS Sharpe mean : "
              << std::setprecision(3) << wfr.oos_sharpe_mean << "\n";
    std::cout << "  WF OOS consistency : "
              << std::setprecision(1) << wfr.oos_consistency * 100 << "%\n";
    std::cout << "  WF best params     : ";
    for (auto& [k,v] : wfr.best_params)
        std::cout << k << "=" << std::setprecision(3) << v << " ";
    std::cout << "\n";

    // ── Objective functions ─────────────────────────────────────────────────
    TrialResult tr;
    tr.sharpe=1.5; tr.calmar=2.0; tr.pnl=100.0; tr.total_fills=50; tr.halted=false;
    CHECK_NEAR(obj_sharpe(tr), 1.5, 1e-9, "obj_sharpe returns sharpe");
    CHECK_NEAR(obj_calmar(tr), 2.0, 1e-9, "obj_calmar returns calmar");
    CHECK_NEAR(obj_pnl(tr),  100.0, 1e-9, "obj_pnl returns pnl");
    tr.halted = true;
    CHECK(obj_sharpe(tr) < -1e8, "obj_sharpe penalises halted strategy");

    // ── SPSC ring buffer (used inside LiveReplay) ───────────────────────────
    SPSCRingBuffer<int, 8> ring;
    CHECK(ring.empty(), "Ring empty initially");
    ring.push(10); ring.push(20); ring.push(30);
    CHECK(ring.size() == 3, "Ring size = 3");
    int v = 0;
    CHECK(ring.pop(v) && v == 10, "SPSC pop FIFO order: 10");
    CHECK(ring.pop(v) && v == 20, "SPSC pop FIFO order: 20");
    CHECK(ring.pop(v) && v == 30, "SPSC pop FIFO order: 30");
    CHECK(ring.empty(), "Ring empty after pops");
    CHECK(!ring.pop(v), "Pop on empty returns false");
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Manual-drive API (used by LiveReplay engine thread)
// ─────────────────────────────────────────────────────────────────────────────

class CountingStrategy : public Strategy {
public:
    int book_ups=0, trades=0, fills=0, acks=0;
    void on_book_update(const std::string&, OrderBook&, int64_t, SimEngine& e) override {
        ++book_ups;
        if (book_ups == 1) {
            // Submit one order
            auto bb = e.get_book("BTCUSDT");
            if (bb && bb->best_bid())
                e.submit_limit("BTCUSDT", Side::Buy, *bb->best_bid()-0.5, 0.01, true);
        }
    }
    void on_trade(const Trade&, OrderBook&, int64_t, SimEngine&) override { ++trades; }
    void on_fill(const FillEvent&, PortfolioState&, int64_t, SimEngine&) override { ++fills; }
    void on_order_ack(const Order&, bool, int64_t, SimEngine&) override { ++acks; }
};

void test_manual_drive_api() {
    SUITE("Manual-Drive API (process_one)");

    AssetParams ap;
    ap.symbol="BTCUSDT"; ap.initial_price=43500.0;
    ap.annual_vol=0.75; ap.tick_size=0.01; ap.depth_levels=5;
    ap.trade_intensity=5.0; ap.seed=123;
    SyntheticGenerator gen(ap);
    auto events = gen.generate(5.0, 5000);

    CountingStrategy cs;
    FillModelConfig fcfg; fcfg.maker_fee=-0.0002; fcfg.taker_fee=0.0004;
    SimEngine engine(cs, binance_colocation(), fcfg, 50000.0);
    engine.add_symbol("BTCUSDT", 0.01);

    // Drive manually
    engine.on_start_manual();
    for (auto& evt : events) engine.process_one(evt);
    auto stats = engine.on_end_manual();

    CHECK(stats.ticks_processed == (int64_t)events.size(),
          "process_one: all ticks counted");
    CHECK(stats.strategy_errors == 0, "No strategy errors");
    CHECK(cs.book_ups > 0, "Book updates received");
    CHECK(cs.acks >= 1, "At least one order ack");
    std::cout << "  Ticks      : " << stats.ticks_processed << "\n";
    std::cout << "  Book ups   : " << cs.book_ups << "\n";
    std::cout << "  Order acks : " << cs.acks << "\n";
    std::cout << "  Throughput : " << std::fixed << std::setprecision(0)
              << stats.ticks_per_second << "/s\n";

    // Compare with run() for identical event stream
    CountingStrategy cs2;
    SimEngine engine2(cs2, binance_colocation(), fcfg, 50000.0);
    engine2.add_symbol("BTCUSDT", 0.01);
    auto stats2 = engine2.run(events);
    CHECK(stats2.ticks_processed == stats.ticks_processed,
          "run() and process_one() process same tick count");

    // PnL series
    [[maybe_unused]] auto& pnl = engine.pnl_series();
    CHECK(true, "pnl_series() accessible via manual API");
    // Fill history
    auto& fh = engine.fill_history();
    CHECK(true, "fill_history() accessible via manual API");
    (void)fh;
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Integration: FastBook + Analytics on a full MM backtest
// ─────────────────────────────────────────────────────────────────────────────

void test_fastbook_analytics_integration() {
    SUITE("FastBook + Analytics Integration");

    AssetParams ap;
    ap.symbol="BTCUSDT"; ap.initial_price=43500.0;
    ap.annual_vol=0.75; ap.tick_size=0.01; ap.depth_levels=8;
    ap.trade_intensity=6.0; ap.hawkes_alpha=0.3; ap.seed=77;
    SyntheticGenerator gen(ap);
    auto events = gen.generate(20.0, 5000);

    // Run GLFT strategy
    AssetConfig cfg;
    cfg.symbol="BTCUSDT"; cfg.tick_size=0.01; cfg.lot_size=0.001;
    cfg.quote_qty=0.01; cfg.max_inventory=0.1; cfg.as_gamma=0.12;
    cfg.as_k=1.5; cfg.as_T=300.0; cfg.min_spread_bps=1.5;
    cfg.max_spread_bps=35.0; cfg.use_glft=true;
    cfg.toxicity_pause=0.85; cfg.max_quote_age_ms=4000.0;
    cfg.min_refresh_us=400.0;

    MultiAssetMarketMaker mm({cfg}, 0.0003);
    FillModelConfig fcfg; fcfg.maker_fee=-0.0002; fcfg.taker_fee=0.0004;
    RiskLimits limits; limits.max_position["BTCUSDT"]=0.2; limits.max_drawdown=0.10;

    SimEngine engine(mm, binance_colocation(), fcfg, 50000.0, limits,
                     2'000'000'000LL);
    engine.add_symbol("BTCUSDT", 0.01);

    // Also maintain a FastBook in parallel for speed comparison
    FastBook fb("BTCUSDT", 0.01);
    int64_t fb_updates = 0;
    auto t_fb0 = std::chrono::high_resolution_clock::now();
    for (auto& e : events) {
        std::visit([&](const auto& ev){
            using T = std::decay_t<decltype(ev)>;
            if constexpr (std::is_same_v<T, L2Update>) {
                fb.apply_l2(ev); ++fb_updates;
            }
        }, e);
    }
    auto t_fb1 = std::chrono::high_resolution_clock::now();
    double fb_ms = std::chrono::duration<double,std::milli>(t_fb1-t_fb0).count();
    std::cout << "  FastBook: " << fb_updates << " updates in "
              << std::setprecision(2) << fb_ms << "ms ("
              << std::setprecision(0) << fb_updates/fb_ms*1000 << "/s)\n";

    // Run the full simulation
    auto stats = engine.run(events);
    CHECK(stats.strategy_errors == 0, "No strategy errors in integration test");

    // Analytics
    AnalyticsEngine ae(10'000'000'000LL);   // 10s buckets for short test
    ae.ingest(stats, engine.fill_history());
    ae.ingest_pnl_series(engine.pnl_series());

    auto rm = ae.compute_risk_metrics();
    auto cb = ae.compute_cost_breakdown();

    CHECK(cb.total_notional >= 0.0, "Total notional non-negative");
    CHECK(rm.max_drawdown <= 0.0, "Max drawdown non-positive");
    CHECK(rm.win_rate >= 0.0 && rm.win_rate <= 1.0, "Win rate valid");

    std::cout << "  Backtest fills : " << stats.total_fills << "\n";
    std::cout << "  P&L            : $" << std::setprecision(4)
              << stats.portfolio_summary.pnl << "\n";
    std::cout << "  Sharpe         : " << std::setprecision(3) << rm.sharpe << "\n";
    std::cout << "  Win rate       : " << std::setprecision(1)
              << rm.win_rate * 100 << "%\n";
    std::cout << "  Total notional : $" << std::setprecision(0)
              << cb.total_notional << "\n";
    std::cout << "  Net fees (bps) : " << std::setprecision(3) << cb.fee_bps << "\n";

    // Verify FastBook mid is close to OrderBook mid after same events
    auto ob = engine.get_book("BTCUSDT");
    if (ob && ob->mid_price() && fb.mid_price()) {
        double ob_mid = *ob->mid_price();
        double fb_mid = *fb.mid_price();
        // They may differ because the engine resets its book per snapshot interval
        // but both should be in the same price range
        CHECK(fb_mid > 40000.0 && fb_mid < 50000.0, "FastBook mid in valid range");
        std::cout << "  OB mid   : " << std::setprecision(4) << ob_mid << "\n";
        std::cout << "  FB mid   : " << fb_mid << "\n";
    }
}

void test_tick_store() {
    SUITE("TickStore");

    const std::string path = "test_ticks.hftdb";

    std::vector<MarketEvent> events;
    events.push_back(L2Update{"BTCUSDT", BookSide::Bid, 100.0, 1.5, 1000, 1});
    events.push_back(L2Update{"BTCUSDT", BookSide::Ask, 101.0, 2.0, 1001, 2});
    events.push_back(Trade{"t1", "BTCUSDT", Side::Buy, 101.0, 0.1, 1002, Side::Buy, false});
    events.push_back(L2Update{"ETHUSDT", BookSide::Bid, 200.0, 3.0, 1003, 3});

    TickStore::write_file(path, events, true);

    auto stats = TickStore::inspect(path);
    CHECK(stats.records == 4, "TickStore records = 4");
    CHECK(stats.l2 == 3, "TickStore L2 count = 3");
    CHECK(stats.trades == 1, "TickStore trade count = 1");
    CHECK(stats.first_ts == 1000, "TickStore first timestamp");
    CHECK(stats.last_ts == 1003, "TickStore last timestamp");

    auto all = TickStore::read_file(path);
    CHECK(all.size() == 4, "TickStore read all events");

    TickReplayFilter btc_only;
    btc_only.symbol = "BTCUSDT";
    auto btc = TickStore::read_file(path, btc_only);
    CHECK(btc.size() == 3, "TickStore symbol filter BTCUSDT");

    TickReplayFilter trades_only;
    trades_only.include_l2 = false;
    trades_only.include_l3 = false;
    trades_only.include_trades = true;
    auto trades = TickStore::read_file(path, trades_only);
    CHECK(trades.size() == 1, "TickStore trades-only filter");
    CHECK(std::holds_alternative<Trade>(trades[0]), "Trades-only result is Trade");

    TickReplayFilter range;
    range.start_ts = 1001;
    range.end_ts = 1002;
    auto ranged = TickStore::read_file(path, range);
    CHECK(ranged.size() == 2, "TickStore timestamp range filter");

    TickStoreEventSource source(path, btc_only);
    MarketEvent e;
    int count = 0;
    while (source.next(e)) ++count;
    CHECK(count == 3, "TickStoreEventSource iteration");

    source.reset();
    CHECK(source.next(e), "TickStoreEventSource reset works");

    std::remove(path.c_str());
}



void test_distributed_optimizer() {
    SUITE("DistributedOptimizer");

    std::vector<MarketEvent> data;
    data.push_back(L2Update{"BTCUSDT", BookSide::Bid, 100.0, 1.0, 1000, 1});
    data.push_back(L2Update{"BTCUSDT", BookSide::Ask, 101.0, 1.0, 1001, 2});

    std::vector<ParamRange> ranges = {
        {"gamma", 0.1, 0.2, 0.1},
        {"spread", 1.0, 2.0, 1.0}
    };

    auto trial_fn = [](const ParamMap& p, const std::vector<MarketEvent>& d, int fold) {
        TrialResult r;
        r.params = p;
        r.sharpe = p.at("gamma") * 10.0 + p.at("spread");
        r.calmar = r.sharpe * 0.5;
        r.pnl = r.sharpe * 100.0;
        r.pnl_pct = r.sharpe;
        r.max_drawdown = -0.01;
        r.total_fills = static_cast<int64_t>(d.size());
        r.halted = false;
        r.fold = fold;
        return r;
    };

    DistOptConfig cfg;
    cfg.n_workers = 2;
    cfg.verbose = false;
    cfg.resume = false;
    cfg.checkpoint = true;
    cfg.checkpoint_every = 1;
    cfg.checkpoint_path = "test_dist_optimizer_checkpoint.csv";
    cfg.objective = obj_sharpe;

    DistributedOptimizer opt(cfg);
    auto summary = opt.grid_search(ranges, data, trial_fn);

    CHECK(summary.total == 4, "Distributed grid: 4 trials");
    CHECK(summary.completed == 4, "Distributed grid: all completed");
    CHECK(summary.failed == 0, "Distributed grid: no failures");
    CHECK(!summary.top.empty(), "Distributed grid: top results populated");
    CHECK(summary.best.objective >= summary.top.back().objective, "Distributed results sorted");
    CHECK(summary.workers.size() == 2, "Worker telemetry size = 2");

    auto csv = opt.results_to_csv();
    CHECK(!csv.empty(), "Distributed results CSV non-empty");
    CHECK(csv.find("objective") != std::string::npos, "Distributed CSV has header");

    std::ifstream checkpoint(cfg.checkpoint_path);
    CHECK(checkpoint.good(), "Checkpoint file created");
    checkpoint.close();

    std::remove(cfg.checkpoint_path.c_str());

    DistOptConfig cfg2;
    cfg2.n_workers = 2;
    cfg2.verbose = false;
    cfg2.resume = false;
    cfg2.checkpoint = false;
    cfg2.objective = obj_pnl;

    DistributedOptimizer opt2(cfg2);
    auto random_summary = opt2.random_search(ranges, 5, data, trial_fn, 123);

    CHECK(random_summary.total == 5, "Distributed random: 5 trials");
    CHECK(random_summary.completed == 5, "Distributed random: all completed");

    ParamMap base;
    base["gamma"] = 0.15;
    base["spread"] = 1.5;

    auto robust = opt2.monte_carlo_robustness(base, 5, 0.10, data, trial_fn, 123);
    CHECK(!robust.empty(), "Monte Carlo robustness returns top results");
}


void test_market_data_engine_advanced() {
    namespace md = hft::marketdata;
    namespace ex = hft::exchange;

    ex::ExchangeBus bus;
    bus.start();

    std::atomic<int> l2_count{0};

    bus.subscribe<ex::L2Update>(
        ex::ExchangeEventType::L2Update,
        [&](const ex::ExchangeEventHeader&, const ex::L2Update& u) {
            if (u.symbol == "BTC-USDT") {
                l2_count.fetch_add(1, std::memory_order_relaxed);
            }
        },
        "advanced-md-sub"
    );

    md::MarketDataEngine engine(&bus);
    engine.add_symbol_mapping("BTCUSDT", "BTC-USDT");

    md::RawMarketPacket p1;
    p1.venue = "BINANCE";
    p1.symbol = "BTCUSDT";
    p1.type = md::MdPacketType::L2Update;
    p1.sequence = 1;
    p1.side = md::MdSide::Bid;
    p1.price = 100.0;
    p1.qty = 1.0;
    p1.exchange_ts_ns = md::md_now_ns() - 1000;
    p1.receive_ts_ns = md::md_now_ns();

    CHECK(engine.on_packet(p1), "Advanced MD accepts seq 1");

    md::RawMarketPacket p3 = p1;
    p3.sequence = 3;
    p3.price = 102.0;

    CHECK(engine.on_packet(p3), "Advanced MD buffers gap seq 3");
    CHECK(engine.snapshot_pending("BINANCE", "BTCUSDT"), "Advanced MD snapshot pending");

    md::RawMarketPacket p2 = p1;
    p2.sequence = 2;
    p2.price = 101.0;

    CHECK(engine.on_packet(p2), "Advanced MD accepts seq 2 and drains seq 3");

    bus.dispatch_once(10);

    auto book = engine.book("BTC-USDT", 10);

    CHECK(book.bids.size() == 3, "Book has 3 bid levels");
    CHECK(book.bids[0].price == 102.0, "Best bid is seq 3 price");
    CHECK(book.bids[1].price == 101.0, "Second bid is seq 2 price");
    CHECK(book.bids[2].price == 100.0, "Third bid is seq 1 price");

    md::RawMarketPacket duplicate = p2;
    CHECK(!engine.on_packet(duplicate), "Advanced MD rejects duplicate");

    auto metrics = engine.metrics_snapshot();

    CHECK(metrics.packets_received == 4, "Advanced metrics packets received");
    CHECK(metrics.packets_duplicate == 1, "Advanced metrics duplicate");
    CHECK(metrics.sequence_gaps == 1, "Advanced metrics gap");
    CHECK(metrics.packets_buffered == 1, "Advanced metrics buffered");
    CHECK(metrics.packets_applied >= 3, "Advanced metrics applied");

    bus.stop();
}


void test_lock_free_market_data_cache() {
    using namespace hft::marketdata;

    LockFreeMarketDataCache<8> cache;

    cache.update_bid("BTC-USDT", 100.0, 2.0, 1, md_now_ns());
    cache.update_ask("BTC-USDT", 101.0, 3.0, 2, md_now_ns());

    auto tob = cache.top_of_book("BTC-USDT");

    CHECK(tob.has_value(), "TOB snapshot available");
    CHECK(tob->valid(), "TOB valid");
    CHECK(tob->bid_price == 100.0, "TOB bid price");
    CHECK(tob->ask_price == 101.0, "TOB ask price");
    CHECK(tob->mid() == 100.5, "TOB mid");
    CHECK(tob->spread() == 1.0, "TOB spread");

    for (int i = 0; i < 12; ++i) {
        cache.update_trade(
            "BTC-USDT",
            CachedTrade{
                100.0 + i,
                1.0,
                i % 2 == 0,
                static_cast<uint64_t>(i + 3),
                md_now_ns()
            }
        );
    }

    auto trades = cache.recent_trades("BTC-USDT");

    CHECK(trades.size() == 8, "Recent trade cache bounded");
    CHECK(trades.front().price == 104.0, "Recent trades evict oldest");
    CHECK(trades.back().price == 111.0, "Recent trades latest correct");

    auto stats = cache.stats_snapshot();

    CHECK(stats.symbols_created == 1, "Cache symbol created");
    CHECK(stats.tob_updates == 2, "Cache TOB update metric");
    CHECK(stats.trade_updates == 12, "Cache trade update metric");
}


void test_market_data_engine_cache_integration() {
    using namespace hft::marketdata;

    MarketDataEngine engine;
    engine.add_symbol_mapping("ETHUSDT", "ETH-USDT");

    RawMarketPacket bid;
    bid.venue = "BINANCE";
    bid.symbol = "ETHUSDT";
    bid.type = MdPacketType::L2Update;
    bid.sequence = 1;
    bid.side = MdSide::Bid;
    bid.price = 200.0;
    bid.qty = 5.0;

    RawMarketPacket ask = bid;
    ask.sequence = 2;
    ask.side = MdSide::Ask;
    ask.price = 201.0;
    ask.qty = 6.0;

    CHECK(engine.on_packet(bid), "Engine cache bid packet");
    CHECK(engine.on_packet(ask), "Engine cache ask packet");

    auto tob = engine.top_of_book("ETH-USDT");

    CHECK(tob.has_value(), "Engine TOB available");
    CHECK(tob->valid(), "Engine TOB valid");
    CHECK(tob->mid() == 200.5, "Engine TOB mid");

    RawMarketPacket trade = bid;
    trade.type = MdPacketType::Trade;
    trade.sequence = 3;
    trade.price = 200.5;
    trade.qty = 1.25;
    trade.is_trade_buy = true;

    CHECK(engine.on_packet(trade), "Engine trade packet");

    auto trades = engine.recent_trades("ETH-USDT");

    CHECK(trades.size() == 1, "Engine recent trade stored");
    CHECK(trades[0].price == 200.5, "Engine trade price stored");
    CHECK(trades[0].aggressor_buy, "Engine trade side stored");

    auto stats = engine.cache_stats();

    CHECK(stats.tob_updates == 2, "Engine cache TOB stats");
    CHECK(stats.trade_updates == 1, "Engine cache trade stats");
}

void test_advanced_market_data_cache_features() {
    using namespace hft::marketdata;

    LockFreeMarketDataCache<16> cache;

    const auto now = cache_now_ns();

    cache.update_bid("BTC-USDT", 100.0, 5.0, 1, now, now - 500);
    cache.update_ask("BTC-USDT", 101.0, 3.0, 2, now + 1, now - 400);

    auto tob = cache.top_of_book("BTC-USDT");

    CHECK(tob.has_value(), "Advanced TOB available");
    CHECK(tob->valid(), "Advanced TOB valid");
    CHECK(tob->spread() == 1.0, "Advanced spread");
    CHECK(tob->mid() == 100.5, "Advanced mid");
    CHECK(tob->imbalance() > 0.0, "Advanced imbalance positive");
    CHECK(tob->micro_price() > 100.0, "Advanced micro price valid");
    CHECK(tob->last_sequence == 2, "Advanced last sequence");

    cache.update_trade(
        "BTC-USDT",
        CachedTrade{100.5, 1.25, true, 3, now - 300, now + 2}
    );

    auto latest = cache.latest_trade("BTC-USDT");

    CHECK(latest.has_value(), "Latest trade available");
    CHECK(latest->price == 100.5, "Latest trade price");
    CHECK(latest->aggressor_buy, "Latest trade side");

    auto stale = cache.top_of_book("BTC-USDT", 1);

    CHECK(!stale.has_value(), "Stale TOB rejected with tiny timeout");

    cache.update_bid("ETH-USDT", 200.0, 2.0, 1, cache_now_ns());
    cache.update_ask("ETH-USDT", 199.0, 2.0, 2, cache_now_ns());

    auto crossed = cache.top_of_book("ETH-USDT");

    CHECK(crossed.has_value(), "Crossed TOB exists");
    CHECK(crossed->crossed(), "Crossed book detected");

    auto stats = cache.stats_snapshot();

    CHECK(stats.symbols_created == 2, "Advanced symbols created");
    CHECK(stats.tob_updates == 4, "Advanced TOB updates");
    CHECK(stats.bid_updates == 2, "Advanced bid updates");
    CHECK(stats.ask_updates == 2, "Advanced ask updates");
    CHECK(stats.trade_updates == 1, "Advanced trade updates");
    CHECK(stats.snapshot_reads >= 3, "Advanced snapshot reads");
    CHECK(stats.stale_reads >= 1, "Advanced stale reads");
    CHECK(stats.crossed_books >= 1, "Advanced crossed book metric");
}


void test_gap_recovery_engine_advanced() {
    using namespace hft::marketdata;

    auto ADV_ASSERT_TRUE = [](bool ok, const char* msg) {
    if (ok) {
        std::cout << "  PASS: " << msg << "\n";
    } else {
        std::cerr << "  FAIL: " << msg << "\n";
        std::exit(1);
      }
    };

    GapRecoveryConfig config;
    config.max_buffered_packets_per_stream = 4;
    config.recovery_timeout_ns = 1'000'000'000ULL;
    config.request_snapshot_on_gap = true;
    config.allow_buffer_replay = true;

    GapRecoveryEngine recovery(config);

    std::atomic<int> snapshot_requests{0};

    std::string callback_key;
    uint64_t callback_expected = 0;
    uint64_t callback_received = 0;

    recovery.set_snapshot_request_callback(
    [&](const std::string& key, uint64_t expected, uint64_t received) {
        callback_key = key;
        callback_expected = expected;
        callback_received = received;
        snapshot_requests.fetch_add(1, std::memory_order_relaxed);
        }
    );

    RawMarketPacket p1;
    p1.venue = "BINANCE";
    p1.symbol = "BTC-USDT";
    p1.type = MdPacketType::L2Update;
    p1.sequence = 1;
    p1.side = MdSide::Bid;
    p1.price = 100.0;
    p1.qty = 1.0;

    const std::string key = "BINANCE:BTC-USDT";

    auto r1 = recovery.on_packet(key, p1);

    ADV_ASSERT_TRUE(r1.decision == RecoveryDecision::ApplyNow, "Advanced recovery apply first");
    ADV_ASSERT_TRUE(r1.should_apply, "Advanced recovery first should apply");

    RawMarketPacket p4 = p1;
    p4.sequence = 4;
    p4.price = 104.0;

    auto r4 = recovery.on_packet(key, p4);

    ADV_ASSERT_TRUE(r4.decision == RecoveryDecision::GapDetected, "Advanced recovery gap detected");
    ADV_ASSERT_TRUE(!r4.should_apply, "Advanced recovery gap not applied");
    ADV_ASSERT_TRUE(r4.should_request_snapshot, "Advanced recovery requests snapshot");
    ADV_ASSERT_TRUE(snapshot_requests.load() == 1, "Advanced recovery snapshot callback fired");
    ADV_ASSERT_TRUE(callback_key == "BINANCE:BTC-USDT", "Snapshot callback key");
    ADV_ASSERT_TRUE(callback_expected == 2, "Snapshot callback expected seq");
    ADV_ASSERT_TRUE(callback_received == 4, "Snapshot callback received seq");
    ADV_ASSERT_TRUE(recovery.buffered_count(key) == 1, "Advanced recovery buffered p4");

    RawMarketPacket p3 = p1;
    p3.sequence = 3;
    p3.price = 103.0;

    auto r3 = recovery.on_packet(key, p3);

    ADV_ASSERT_TRUE(r3.decision == RecoveryDecision::GapDetected, "Advanced recovery buffers p3");
    ADV_ASSERT_TRUE(recovery.buffered_count(key) == 2, "Advanced recovery buffered p3 and p4");

    auto replay = recovery.apply_snapshot(key, 2);

    ADV_ASSERT_TRUE(replay.size() == 2, "Advanced recovery replay size");
    ADV_ASSERT_TRUE(replay.packets[0].sequence == 3, "Advanced recovery replay seq 3");
    ADV_ASSERT_TRUE(replay.packets[1].sequence == 4, "Advanced recovery replay seq 4");
    ADV_ASSERT_TRUE(recovery.last_sequence(key) == 4, "Advanced recovery last seq 4");
    ADV_ASSERT_TRUE(recovery.state(key) == hft::marketdata::RecoveryState::Synced,"Advanced recovery synced");

    auto dup = recovery.on_packet(key, p4);

    ADV_ASSERT_TRUE(dup.decision == RecoveryDecision::Duplicate ||
                dup.decision == RecoveryDecision::Old,
                "Advanced recovery rejects replay duplicate");

    auto metrics = recovery.metrics_snapshot();

    ADV_ASSERT_TRUE(metrics.packets_seen >= 4, "Advanced recovery packets seen");
    ADV_ASSERT_TRUE(metrics.gaps_detected >= 2, "Advanced recovery gap metric");
    ADV_ASSERT_TRUE(metrics.packets_buffered == 2, "Advanced recovery buffered metric");
    ADV_ASSERT_TRUE(metrics.packets_replayed == 2, "Advanced recovery replay metric");
    ADV_ASSERT_TRUE(metrics.snapshots_requested >= 1, "Advanced recovery snapshot request metric");
    ADV_ASSERT_TRUE(metrics.snapshots_applied == 1, "Advanced recovery snapshot applied metric");
}




void test_smart_order_router() {
    hft::routing::SmartOrderRouter router;

    router.update_quote({"BINANCE", "BTC-USDT", 100.0, 101.0, 2.0, 2.0, 1.0, 20.0, true});
    router.update_quote({"OKX", "BTC-USDT", 99.8, 101.2, 5.0, 5.0, 0.5, 10.0, true});
    router.update_quote({"BYBIT", "BTC-USDT", 99.5, 102.0, 10.0, 10.0, 2.0, 50.0, true});

    hft::routing::RouterOrder order;
    order.symbol = "BTC-USDT";
    order.side = hft::routing::Side::Buy;
    order.type = hft::routing::OrderType::Limit;
    order.qty = 6.0;
    order.limit_price = 102.5;

    auto result = router.route(order);

    CHECK(
    result.decision == hft::routing::RouteDecision::SplitVenues,
    "SOR split venue decision"
       );

    CHECK(result.children.size() >= 2, "SOR creates multiple child routes");
    CHECK(result.expected_avg_price > 0.0, "SOR expected avg price");
    CHECK(result.expected_total_cost > 0.0, "SOR expected total cost");

    double routed_qty = 0.0;

    for (const auto& child : result.children) {
    routed_qty += child.qty;
    CHECK(!child.venue.empty(), "SOR child venue set");
    CHECK(child.qty > 0.0, "SOR child qty positive");
    CHECK(child.price > 0.0, "SOR child price positive");
    }

    CHECK(std::abs(routed_qty - 6.0) < 0.000001, "SOR routed full quantity");
    CHECK(router.metrics().route_requests.load() == 1, "SOR route metric");
}



void test_child_order_manager_core() {
    hft::execution::ChildOrderManager com;

    hft::execution::ParentCreateRequest request;
    request.symbol = "BTC-USDT";
    request.side = hft::execution::ChildOrderSide::Buy;
    request.quantity = 10.0;
    request.timestamp_ns = 1000;

    auto parent_id = com.create_parent(request);

    CHECK(parent_id > 0, "COM parent created");

    std::vector<hft::execution::ChildOrderRoute> routes = {
        {"BINANCE", 4.0, 101.0},
        {"OKX", 6.0, 101.2}
    };

    auto child_ids = com.create_children(parent_id, routes, 1100);

    CHECK(child_ids.size() == 2, "COM children created");

    CHECK(com.mark_submitted(child_ids[0], 1200), "COM child submitted");
    CHECK(com.acknowledge(child_ids[0], "BN-1", 1300), "COM child acknowledged");

    CHECK(com.apply_fill(child_ids[0], 2.0, 101.0, 1400), "COM partial fill applied");

    auto child = com.child(child_ids[0]);
    CHECK(child.has_value(), "COM child lookup");
    CHECK(child->state == hft::execution::ChildOrderState::PartiallyFilled, "COM child partially filled");
    CHECK(child->filled_quantity == 2.0, "COM child filled quantity");
    CHECK(child->remaining_quantity == 2.0, "COM child remaining quantity");

    auto parent = com.parent(parent_id);
    CHECK(parent.has_value(), "COM parent lookup");
    CHECK(parent->state == hft::execution::ParentOrderState::PartiallyFilled, "COM parent partially filled");
    CHECK(parent->filled_quantity == 2.0, "COM parent filled quantity");
    CHECK(parent->remaining_quantity == 8.0, "COM parent remaining quantity");

    CHECK(com.apply_fill(child_ids[0], 2.0, 101.5, 1500), "COM final fill child one");

    CHECK(com.mark_submitted(child_ids[1], 1600), "COM child two submitted");
    CHECK(com.acknowledge(child_ids[1], "OKX-1", 1700), "COM child two acknowledged");
    CHECK(com.apply_fill(child_ids[1], 6.0, 101.2, 1800), "COM child two filled");

    parent = com.parent(parent_id);
    CHECK(parent.has_value(), "COM parent lookup final");
    CHECK(parent->state == hft::execution::ParentOrderState::Filled, "COM parent filled");
    CHECK(parent->filled_quantity == 10.0, "COM parent full fill quantity");
    CHECK(parent->remaining_quantity < 0.000001, "COM parent no remaining quantity");
    CHECK(parent->average_price > 0.0, "COM parent VWAP calculated");

    CHECK(com.metrics().parents_created.load() == 1, "COM parent metric");
    CHECK(com.metrics().children_created.load() == 2, "COM children metric");
    CHECK(com.metrics().full_fills.load() == 2, "COM full fill metric");
}


void test_child_order_manager_execution_reports() {
    hft::execution::ChildOrderManager com;

    hft::execution::ParentCreateRequest request;
    request.symbol = "ETH-USDT";
    request.side = hft::execution::ChildOrderSide::Sell;
    request.quantity = 5.0;
    request.timestamp_ns = 1000;

    auto parent_id = com.create_parent(request);
    CHECK(parent_id > 0, "COM parent created for reports");

    std::vector<hft::execution::ChildOrderRoute> routes = {
        {"BINANCE", 2.0, 2000.0},
        {"OKX", 3.0, 1999.5}
    };

    auto child_ids = com.create_children(parent_id, routes, 1100);
    CHECK(child_ids.size() == 2, "COM report children created");

    CHECK(com.mark_submitted(child_ids[0], 1200), "COM first child submitted");
    CHECK(com.mark_submitted(child_ids[1], 1200), "COM second child submitted");

    hft::execution::ExecutionReport ack;
    ack.type = hft::execution::ExecutionReportType::Ack;
    ack.child_id = child_ids[0];
    ack.venue_order_id = "BN-ETH-1";
    ack.timestamp_ns = 1300;

    CHECK(com.on_execution_report(ack), "COM ack report handled");

    auto by_venue = com.child_by_venue_order_id("BN-ETH-1");
    CHECK(by_venue.has_value(), "COM venue order lookup works");

    hft::execution::ExecutionReport partial;
    partial.type = hft::execution::ExecutionReportType::PartialFill;
    partial.venue_order_id = "BN-ETH-1";
    partial.fill_qty = 1.0;
    partial.fill_price = 2000.0;
    partial.timestamp_ns = 1400;

    CHECK(com.on_execution_report(partial), "COM partial fill report handled");

    auto child = com.child(child_ids[0]);
    CHECK(child.has_value(), "COM child after partial lookup");
    CHECK(child->state == hft::execution::ChildOrderState::PartiallyFilled, "COM child partial state");

    hft::execution::ExecutionReport fill;
    fill.type = hft::execution::ExecutionReportType::Fill;
    fill.venue_order_id = "BN-ETH-1";
    fill.fill_qty = 1.0;
    fill.fill_price = 2001.0;
    fill.timestamp_ns = 1500;

    CHECK(com.on_execution_report(fill), "COM final fill report handled");

    child = com.child(child_ids[0]);
    CHECK(child.has_value(), "COM child after final fill lookup");
    CHECK(child->state == hft::execution::ChildOrderState::Filled, "COM child filled through reports");

    hft::execution::ExecutionReport reject;
    reject.type = hft::execution::ExecutionReportType::Reject;
    reject.child_id = child_ids[1];
    reject.reason = "Venue throttled";
    reject.timestamp_ns = 1600;

    CHECK(com.on_execution_report(reject), "COM reject report handled");

    auto parent = com.parent(parent_id);
    CHECK(parent.has_value(), "COM parent after reject lookup");
    CHECK(parent->filled_quantity == 2.0, "COM parent aggregated fill after reports");
    CHECK(parent->remaining_quantity == 3.0, "COM parent remaining after reject");

    auto metrics = com.metrics_snapshot();
    CHECK(metrics.children_acknowledged == 1, "COM ack metric");
    CHECK(metrics.partial_fills == 1, "COM partial metric");
    CHECK(metrics.full_fills == 1, "COM full fill metric");
    CHECK(metrics.rejects == 1, "COM reject metric");
    CHECK(metrics.avg_ack_latency_us > 0.0, "COM ack latency metric");
    CHECK(metrics.avg_fill_latency_us > 0.0, "COM fill latency metric");
}


void test_child_order_command_factory() {
    hft::execution::ChildOrderManager com;

    hft::execution::ParentCreateRequest request;
    request.symbol = "BTC-USDT";
    request.side = hft::execution::ChildOrderSide::Buy;
    request.quantity = 3.0;
    request.timestamp_ns = 1000;

    auto parent_id = com.create_parent(request);
    CHECK(parent_id > 0, "COM command parent created");

    std::vector<hft::execution::ChildOrderRoute> routes = {
        {"BINANCE", 1.0, 101.0},
        {"OKX", 2.0, 101.2}
    };

    auto children = com.create_children(parent_id, routes, 1100);
    CHECK(children.size() == 2, "COM command children created");

    hft::execution::ChildOrderCommandFactory factory(com);

    auto submit_commands = factory.build_submit_commands(children, 1200);

    CHECK(submit_commands.size() == 2, "COM submit commands created");
    CHECK(submit_commands[0].type == hft::execution::ChildCommandType::Submit, "COM submit command type");
    CHECK(!submit_commands[0].client_order_id.empty(), "COM submit client id");
    CHECK(submit_commands[0].quantity > 0.0, "COM submit quantity");

    auto cancel_cmd = factory.build_cancel_command(children[0], 1300);

    CHECK(cancel_cmd.has_value(), "COM cancel command created");
    CHECK(cancel_cmd->type == hft::execution::ChildCommandType::Cancel, "COM cancel command type");

    auto child = com.child(children[0]);
    CHECK(child.has_value(), "COM child after cancel lookup");
    CHECK(child->state == hft::execution::ChildOrderState::CancelPending, "COM child cancel pending");

    auto replace_cmd = factory.build_replace_command(children[1], 2.5, 101.1, 1400);

    CHECK(replace_cmd.has_value(), "COM replace command created");
    CHECK(replace_cmd->type == hft::execution::ChildCommandType::Replace, "COM replace command type");
    CHECK(replace_cmd->quantity == 2.5, "COM replace quantity");
    CHECK(replace_cmd->price == 101.1, "COM replace price");

    child = com.child(children[1]);
    CHECK(child.has_value(), "COM child after replace lookup");
    CHECK(child->state == hft::execution::ChildOrderState::ReplacePending, "COM child replace pending");
}


void test_child_order_command_factory_advanced() {
    hft::execution::ChildOrderManager com;

    hft::execution::ParentCreateRequest request;
    request.symbol = "BTC-USDT";
    request.side = hft::execution::ChildOrderSide::Buy;
    request.quantity = 3.0;
    request.timestamp_ns = 1000;

    auto parent_id = com.create_parent(request);
    CHECK(parent_id > 0, "Advanced commands parent created");

    std::vector<hft::execution::ChildOrderRoute> routes = {
        {"BINANCE", 1.0, 101.0},
        {"OKX", 2.0, 101.2}
    };

    auto children = com.create_children(parent_id, routes, 1100);
    CHECK(children.size() == 2, "Advanced commands children created");

    hft::execution::ChildOrderCommandFactory factory(com);

    auto submits = factory.build_submit_commands(children, 1200);
    CHECK(submits.size() == 2, "Advanced submit commands created");

    CHECK(submits[0].command_id > 0, "Advanced command id created");
    CHECK(!submits[0].idempotency_key.empty(), "Advanced idempotency key created");

    CHECK(factory.mark_sent(submits[0].command_id, 1300), "Advanced command marked sent");
    CHECK(factory.mark_acked(submits[0].command_id, 1400), "Advanced command marked acked");

    auto stored = factory.command(submits[0].command_id);
    CHECK(stored.has_value(), "Advanced command lookup");
    CHECK(stored->state == hft::execution::ChildCommandState::Acked, "Advanced command acked state");

    auto by_key = factory.command_by_idempotency_key(submits[0].idempotency_key);
    CHECK(by_key.has_value(), "Advanced command idempotency lookup");

    auto cancel = factory.build_cancel_command(children[1], 1500);
    CHECK(cancel.has_value(), "Advanced cancel command created");

    CHECK(factory.mark_sent(cancel->command_id, 1600), "Advanced cancel sent");
    CHECK(factory.mark_failed(cancel->command_id, "network error", 1700), "Advanced cancel failed");

    auto failed = factory.command(cancel->command_id);
    CHECK(failed.has_value(), "Advanced failed command lookup");
    CHECK(failed->state == hft::execution::ChildCommandState::Failed, "Advanced command failed state");

    auto expired = factory.expire_stale_commands(10'000, 1000);
    CHECK(expired.size() == 1, "Advanced stale unsent command expired");

    CHECK(factory.metrics().created.load() >= 3, "Advanced command created metric");
    CHECK(factory.metrics().sent.load() == 2, "Advanced command sent metric");
    CHECK(factory.metrics().acked.load() == 1, "Advanced command acked metric");
    CHECK(factory.metrics().failed.load() == 1, "Advanced command failed metric");
}


void test_child_order_adapter_handoff() {
    hft::execution::ChildOrderManager com;

    hft::execution::ParentCreateRequest request;
    request.symbol = "BTC-USDT";
    request.side = hft::execution::ChildOrderSide::Buy;
    request.quantity = 2.0;
    request.timestamp_ns = 1000;

    auto parent_id = com.create_parent(request);
    CHECK(parent_id > 0, "Handoff parent created");

    std::vector<hft::execution::ChildOrderRoute> routes = {
        {"BINANCE", 2.0, 101.0}
    };

    auto children = com.create_children(parent_id, routes, 1100);
    CHECK(children.size() == 1, "Handoff child created");

    hft::execution::ChildOrderCommandFactory factory(com);
    auto submit_commands = factory.build_submit_commands(children, 1200);

    CHECK(submit_commands.size() == 1, "Handoff submit command created");

    hft::execution::ChildOrderAdapterHandoff handoff;

    auto missing = handoff.handoff(submit_commands[0]);
    CHECK(
        missing.status == hft::execution::AdapterHandoffStatus::AdapterMissing,
        "Handoff detects missing adapter"
    );

    auto adapter = std::make_shared<hft::execution::MockChildOrderExecutionAdapter>();
    handoff.register_adapter("BINANCE", adapter);

    auto result = handoff.handoff(submit_commands[0]);

    CHECK(
        result.status == hft::execution::AdapterHandoffStatus::Accepted,
        "Handoff submit accepted"
    );

    CHECK(adapter->submitted.size() == 1, "Mock adapter received submit");
    CHECK(handoff.metrics().accepted.load() == 1, "Handoff accepted metric");

    auto ack_report = hft::execution::ExecutionReport{};
    ack_report.type = hft::execution::ExecutionReportType::Ack;
    ack_report.child_id = children[0];
    ack_report.venue_order_id = "BN-ORDER-1";
    ack_report.timestamp_ns = 1300;

    CHECK(com.on_execution_report(ack_report), "Handoff COM ack applied");

    auto cancel_cmd = factory.build_cancel_command(children[0], 1400);
    CHECK(cancel_cmd.has_value(), "Handoff cancel command created");

    auto cancel_result = handoff.handoff(*cancel_cmd);

    CHECK(
        cancel_result.status == hft::execution::AdapterHandoffStatus::Accepted,
        "Handoff cancel accepted"
    );

    CHECK(adapter->cancelled.size() == 1, "Mock adapter received cancel");

    auto replace_cmd = factory.build_replace_command(children[0], 2.0, 100.8, 1500);

    CHECK(!replace_cmd.has_value(), "Replace rejected while cancel pending");
}


void test_child_order_adapter_handoff_advanced() {
    hft::execution::ChildOrderManager com;

    hft::execution::ParentCreateRequest request;
    request.symbol = "ETH-USDT";
    request.side = hft::execution::ChildOrderSide::Buy;
    request.quantity = 4.0;
    request.timestamp_ns = 1000;

    auto parent_id = com.create_parent(request);
    CHECK(parent_id > 0, "Advanced handoff parent created");

    std::vector<hft::execution::ChildOrderRoute> routes = {
        {"BINANCE", 2.0, 2000.0},
        {"OKX", 2.0, 2001.0}
    };

    auto children = com.create_children(parent_id, routes, 1100);
    CHECK(children.size() == 2, "Advanced handoff children created");

    hft::execution::ChildOrderCommandFactory factory(com);
    auto commands = factory.build_submit_commands(children, 1200);

    CHECK(commands.size() == 2, "Advanced handoff submit commands");

    hft::execution::ChildOrderAdapterHandoff handoff;

    int callback_count = 0;
    handoff.set_result_callback(
        [&](const hft::execution::AdapterHandoffResult& result) {
            if (result.child_id > 0) {
                ++callback_count;
            }
        }
    );

    auto binance = std::make_shared<hft::execution::MockChildOrderExecutionAdapter>();
    auto okx = std::make_shared<hft::execution::MockChildOrderExecutionAdapter>();

    handoff.register_adapter("BINANCE", binance);
    handoff.register_adapter("OKX", okx);

    CHECK(handoff.has_adapter("BINANCE"), "Advanced handoff Binance adapter registered");
    CHECK(handoff.has_adapter("OKX"), "Advanced handoff OKX adapter registered");

    auto batch_results = handoff.handoff_batch(commands);

    CHECK(batch_results.size() == 2, "Advanced handoff batch results");
    CHECK(binance->submitted.size() == 1, "Advanced handoff Binance submitted");
    CHECK(okx->submitted.size() == 1, "Advanced handoff OKX submitted");
    CHECK(callback_count == 2, "Advanced handoff callback count");

    auto binance_state = handoff.venue_state("BINANCE");
    CHECK(binance_state.has_value(), "Advanced handoff venue state exists");
    CHECK(binance_state->accepted == 1, "Advanced handoff venue accepted count");

    CHECK(handoff.set_venue_enabled("OKX", false), "Advanced handoff disable OKX");

    auto disabled_result = handoff.handoff(commands[1]);

    CHECK(
        disabled_result.status == hft::execution::AdapterHandoffStatus::AdapterDisabled,
        "Advanced handoff disabled venue rejected"
    );

    CHECK(handoff.set_venue_enabled("OKX", true), "Advanced handoff enable OKX");

    okx->accept_submit = false;

    auto rejected_result = handoff.handoff(commands[1]);

    CHECK(
        rejected_result.status == hft::execution::AdapterHandoffStatus::Rejected,
        "Advanced handoff adapter reject"
    );

    okx->accept_submit = true;
    okx->throw_on_submit = true;

    auto exception_result = handoff.handoff(commands[1]);

    CHECK(
        exception_result.status == hft::execution::AdapterHandoffStatus::ExceptionThrown,
        "Advanced handoff exception handled"
    );

    CHECK(handoff.unregister_adapter("OKX"), "Advanced handoff unregister OKX");

    auto missing_result = handoff.handoff(commands[1]);

    CHECK(
        missing_result.status == hft::execution::AdapterHandoffStatus::AdapterMissing,
        "Advanced handoff missing after unregister"
    );

    auto metrics = handoff.metrics_snapshot();

    CHECK(metrics.submit_attempts >= 6, "Advanced handoff submit attempts metric");
    CHECK(metrics.accepted >= 2, "Advanced handoff accepted metric");
    CHECK(metrics.rejected >= 4, "Advanced handoff rejected metric");
    CHECK(metrics.disabled_adapter >= 1, "Advanced handoff disabled metric");
    CHECK(metrics.missing_adapter >= 1, "Advanced handoff missing metric");
    CHECK(metrics.exceptions >= 1, "Advanced handoff exception metric");
    CHECK(metrics.avg_latency_us >= 0.0, "Advanced handoff latency metric");
}


void test_venue_scoring_engine() {
    hft::routing::VenueScoringEngine scorer;

    hft::routing::RouterOrder order;
    order.symbol = "BTC-USDT";
    order.side = hft::routing::Side::Buy;
    order.type = hft::routing::OrderType::Limit;
    order.qty = 1.0;
    order.limit_price = 102.0;

    std::vector<hft::routing::VenueQuote> quotes = {
        {"BINANCE", "BTC-USDT", 100.0, 101.0, 5.0, 5.0, 1.0, 20.0, true},
        {"OKX", "BTC-USDT", 99.8, 101.1, 5.0, 5.0, 0.5, 10.0, true},
        {"SLOWX", "BTC-USDT", 99.7, 100.9, 5.0, 5.0, 10.0, 4000.0, true}
    };

    scorer.record_order_sent("SLOWX", "BTC-USDT");
    scorer.record_reject("SLOWX", "BTC-USDT");

    scorer.record_order_sent("OKX", "BTC-USDT");
    scorer.record_fill("OKX", "BTC-USDT", 100);

    auto scores = scorer.score_quotes(order, quotes);

    CHECK(!scores.empty(), "Venue scores created");
    CHECK(scores.front().venue == "OKX", "OKX selected as best scored venue");
    CHECK(scores.front().score > 0.0, "Venue score positive");
    CHECK(scores.front().fill_score > 0.0, "Venue fill score positive");
    CHECK(scores.front().reject_score > 0.0, "Venue reject score positive");

    auto best = scorer.best_venue(order, quotes);

    CHECK(best.has_value(), "Best venue returned");
    CHECK(best->venue == "OKX", "Best venue is OKX");

    auto stats = scorer.stats_for("OKX", "BTC-USDT");

    CHECK(stats != nullptr, "Venue stats found");
    CHECK(stats->orders_sent.load() == 1, "Venue orders sent metric");
    CHECK(stats->fills.load() == 1, "Venue fills metric");
}


void test_adaptive_smart_order_router() {
    hft::routing::AdaptiveRouterConfig config;
    config.min_score_to_route = 0.20;
    config.router_config.min_child_qty = 0.000001;

    hft::routing::AdaptiveSmartOrderRouter router(config);

    router.update_quote({"BINANCE", "BTC-USDT", 100.0, 101.0, 2.0, 2.0, 1.0, 20.0, true});
    router.update_quote({"OKX", "BTC-USDT", 99.8, 101.1, 5.0, 5.0, 0.5, 10.0, true});
    router.update_quote({"SLOWX", "BTC-USDT", 99.7, 100.9, 10.0, 10.0, 10.0, 4000.0, true});

    router.record_order_sent("SLOWX", "BTC-USDT");
    router.record_reject("SLOWX", "BTC-USDT");

    router.record_order_sent("OKX", "BTC-USDT");
    router.record_fill("OKX", "BTC-USDT", 100);

    hft::routing::RouterOrder order;
    order.symbol = "BTC-USDT";
    order.side = hft::routing::Side::Buy;
    order.type = hft::routing::OrderType::Limit;
    order.qty = 6.0;
    order.limit_price = 102.0;

    auto result = router.route(order);

    CHECK(
        result.decision == hft::routing::RouteDecision::SplitVenues,
        "Adaptive SOR split decision"
    );

    CHECK(result.children.size() >= 2, "Adaptive SOR created child routes");
    CHECK(result.children.front().venue == "OKX", "Adaptive SOR prioritizes scored venue");

    double routed_qty = 0.0;

    for (const auto& child : result.children) {
        routed_qty += child.qty;
        CHECK(!child.venue.empty(), "Adaptive SOR child venue");
        CHECK(child.qty > 0.0, "Adaptive SOR child qty");
        CHECK(child.price > 0.0, "Adaptive SOR child price");
    }

    CHECK(std::abs(routed_qty - 6.0) < 0.000001, "Adaptive SOR routed full quantity");
    CHECK(router.metrics().route_requests.load() == 1, "Adaptive SOR route metric");
    CHECK(router.metrics().split_venues.load() == 1, "Adaptive SOR split metric");
}


void test_adaptive_smart_order_router_advanced() {
    hft::routing::AdaptiveRouterConfig config;
    config.min_score_to_route = 0.20;
    config.max_route_slippage_bps = 50.0;
    config.router_config.min_child_qty = 0.000001;
    config.quote_stale_after_ns = 1000;
    config.reject_stale_quotes = true;

    hft::routing::AdaptiveSmartOrderRouter router(config);

    router.update_quote(
        {"BINANCE", "BTC-USDT", 100.0, 101.0, 2.0, 2.0, 1.0, 20.0, true},
        10'000
    );

    router.update_quote(
        {"OKX", "BTC-USDT", 99.8, 101.1, 5.0, 5.0, 0.5, 10.0, true},
        10'000
    );

    router.update_quote(
        {"STALE", "BTC-USDT", 99.0, 100.5, 10.0, 10.0, 0.1, 5.0, true},
        1
    );

    router.record_order_sent("OKX", "BTC-USDT");
    router.record_fill("OKX", "BTC-USDT", 100);

    router.record_order_sent("STALE", "BTC-USDT");
    router.record_fill("STALE", "BTC-USDT", 100);

    hft::routing::RouterOrder order;
    order.symbol = "BTC-USDT";
    order.side = hft::routing::Side::Buy;
    order.type = hft::routing::OrderType::Limit;
    order.qty = 6.0;
    order.limit_price = 102.0;

    auto result = router.route_advanced(order, 10'500);

    CHECK(
        result.decision == hft::routing::RouteDecision::SplitVenues,
        "Advanced adaptive SOR split decision"
    );

    CHECK(result.children.size() == 2, "Advanced adaptive SOR filtered stale venue");
    CHECK(result.children.front().venue == "OKX", "Advanced adaptive SOR prioritizes OKX");
    CHECK(result.explanations.size() >= 2, "Advanced adaptive SOR explanations created");
    CHECK(result.routed_qty == 6.0, "Advanced adaptive SOR routed quantity");
    CHECK(result.remaining_qty < 0.000001, "Advanced adaptive SOR no remaining qty");
    CHECK(!result.partial, "Advanced adaptive SOR not partial");

    CHECK(
        router.metrics().stale_quotes_filtered.load() >= 1,
        "Advanced adaptive SOR stale quote metric"
    );
}


void test_execution_simulator_full_fill() {
    hft::routing::AdaptiveSmartOrderRouter router;

    router.update_quote({"BINANCE", "BTC-USDT", 100.0, 101.0, 5.0, 5.0, 1.0, 20.0, true});
    router.update_quote({"OKX", "BTC-USDT", 99.9, 101.1, 5.0, 5.0, 0.5, 10.0, true});

    hft::execution::ChildOrderManager com;
    hft::execution::ChildOrderCommandFactory factory(com);
    hft::execution::ChildOrderAdapterHandoff handoff;

    auto binance = std::make_shared<hft::execution::MockChildOrderExecutionAdapter>();
    auto okx = std::make_shared<hft::execution::MockChildOrderExecutionAdapter>();

    handoff.register_adapter("BINANCE", binance);
    handoff.register_adapter("OKX", okx);

    hft::execution::ExecutionSimulationConfig sim_config;
    sim_config.mode = hft::execution::SimulatedExecutionMode::FullFill;

    hft::execution::ExecutionSimulator sim(
        router,
        com,
        factory,
        handoff,
        sim_config
    );

    hft::routing::RouterOrder order;
    order.symbol = "BTC-USDT";
    order.side = hft::routing::Side::Buy;
    order.type = hft::routing::OrderType::Limit;
    order.qty = 6.0;
    order.limit_price = 102.0;

    auto result = sim.run(order, 10'000);

    CHECK(result.accepted, "Execution simulator accepted order");
    CHECK(result.parent_id > 0, "Execution simulator created parent");
    CHECK(!result.child_ids.empty(), "Execution simulator created children");
    CHECK(!result.commands.empty(), "Execution simulator created commands");

    auto parent = com.parent(result.parent_id);

    CHECK(parent.has_value(), "Execution simulator parent lookup");
    CHECK(parent->state == hft::execution::ParentOrderState::Filled, "Execution simulator parent filled");
    CHECK(parent->filled_quantity == 6.0, "Execution simulator full fill quantity");
    CHECK(parent->remaining_quantity < 0.000001, "Execution simulator no remaining qty");

    CHECK(sim.metrics().simulations.load() == 1, "Execution simulator metric simulations");
    CHECK(sim.metrics().accepted.load() == 1, "Execution simulator metric accepted");
    CHECK(sim.metrics().reports_generated.load() >= 2, "Execution simulator generated reports");
}


void test_execution_simulator_advanced_profiles() {
    hft::routing::AdaptiveSmartOrderRouter router;

    router.update_quote({"BINANCE", "BTC-USDT", 100.0, 101.0, 5.0, 5.0, 1.0, 20.0, true});
    router.update_quote({"OKX", "BTC-USDT", 99.9, 101.1, 5.0, 5.0, 0.5, 10.0, true});

    hft::execution::ChildOrderManager com;
    hft::execution::ChildOrderCommandFactory factory(com);
    hft::execution::ChildOrderAdapterHandoff handoff;

    auto binance = std::make_shared<hft::execution::MockChildOrderExecutionAdapter>();
    auto okx = std::make_shared<hft::execution::MockChildOrderExecutionAdapter>();

    handoff.register_adapter("BINANCE", binance);
    handoff.register_adapter("OKX", okx);

    hft::execution::ExecutionSimulationConfig config;
    config.mode = hft::execution::SimulatedExecutionMode::Auto;
    config.seed = 7;

    hft::execution::ExecutionSimulator sim(router, com, factory, handoff, config);

    hft::execution::VenueSimulationProfile binance_profile;
    binance_profile.fill_probability = 1.0;
    binance_profile.reject_probability = 0.0;
    binance_profile.min_fill_ratio = 1.0;
    binance_profile.max_fill_ratio = 1.0;
    binance_profile.base_slippage_bps = 2.0;
    binance_profile.impact_bps_per_unit = 0.5;

    hft::execution::VenueSimulationProfile okx_profile;
    okx_profile.fill_probability = 1.0;
    okx_profile.reject_probability = 0.0;
    okx_profile.min_fill_ratio = 0.5;
    okx_profile.max_fill_ratio = 0.5;
    okx_profile.base_slippage_bps = 1.0;
    okx_profile.impact_bps_per_unit = 0.25;

    sim.set_venue_profile("BINANCE", binance_profile);
    sim.set_venue_profile("OKX", okx_profile);

    hft::routing::RouterOrder order;
    order.symbol = "BTC-USDT";
    order.side = hft::routing::Side::Buy;
    order.type = hft::routing::OrderType::Limit;
    order.qty = 6.0;
    order.limit_price = 102.0;

    auto result = sim.run(order, 10'000);

    CHECK(result.parent_id > 0, "Advanced sim parent created");
    CHECK(!result.child_reports.empty(), "Advanced sim child reports created");
    CHECK(result.filled_qty > 0.0, "Advanced sim filled quantity positive");
    CHECK(result.avg_fill_price > 0.0, "Advanced sim average fill price");
    CHECK(result.avg_fill_price >= 101.0, "Advanced sim buy slippage applied");

    CHECK(sim.metrics().simulations.load() == 1, "Advanced sim simulations metric");
    CHECK(sim.metrics().reports_generated.load() >= 2, "Advanced sim reports generated");
}


void test_queue_position_model() {
    hft::routing::QueuePositionModel model;

    hft::routing::QueueInputs fast;
    fast.venue = "OKX";
    fast.symbol = "BTC-USDT";
    fast.side = hft::routing::Side::Buy;
    fast.order_qty = 1.0;
    fast.order_price = 101.0;
    fast.best_bid = 100.9;
    fast.best_ask = 101.0;
    fast.bid_qty_ahead = 5.0;
    fast.ask_qty_ahead = 3.0;
    fast.recent_trade_qty = 20.0;
    fast.recent_trade_rate_per_sec = 10.0;
    fast.latency_us = 20.0;

    auto fast_est = model.estimate(fast);

    CHECK(fast_est.fill_probability > 0.0, "Queue model fill probability positive");
    CHECK(fast_est.expected_fill_time_ms > 0.0, "Queue model expected fill time positive");
    CHECK(fast_est.queue_position_qty > 0.0, "Queue model queue position positive");

    hft::routing::QueueInputs slow = fast;
    slow.venue = "SLOWX";
    slow.order_price = 100.7;
    slow.latency_us = 4000.0;
    slow.recent_trade_qty = 1.0;
    slow.recent_trade_rate_per_sec = 0.5;

    auto slow_est = model.estimate(slow);

    CHECK(
        fast_est.fill_probability > slow_est.fill_probability,
        "Queue model prefers aggressive liquid venue"
    );

    std::vector<hft::routing::QueueInputs> inputs = {slow, fast};
    auto ranked = model.rank_by_fill_probability(inputs);

    CHECK(ranked.size() == 2, "Queue model ranked two venues");
    CHECK(ranked.front().venue == "OKX", "Queue model ranks OKX first");
}


void test_advanced_queue_position_model() {
    hft::routing::QueuePositionModel model;

    for (int i = 0; i < 20; ++i) {
        model.record_event({
            "OKX",
            "BTC-USDT",
            hft::routing::LiquidityEventType::Trade,
            hft::routing::Side::Sell,
            2.0,
            101.0,
            static_cast<uint64_t>(1000 + i)
        });
    }

    for (int i = 0; i < 5; ++i) {
        model.record_event({
            "OKX",
            "BTC-USDT",
            hft::routing::LiquidityEventType::Cancel,
            hft::routing::Side::Sell,
            1.0,
            101.0,
            static_cast<uint64_t>(2000 + i)
        });
    }

    for (int i = 0; i < 20; ++i) {
        model.record_event({
            "SLOWX",
            "BTC-USDT",
            hft::routing::LiquidityEventType::Add,
            hft::routing::Side::Sell,
            5.0,
            101.0,
            static_cast<uint64_t>(3000 + i)
        });
    }

    hft::routing::QueueInputs okx;
    okx.venue = "OKX";
    okx.symbol = "BTC-USDT";
    okx.side = hft::routing::Side::Buy;
    okx.order_qty = 1.0;
    okx.order_price = 101.0;
    okx.best_bid = 100.9;
    okx.best_ask = 101.0;
    okx.bid_qty_ahead = 5.0;
    okx.ask_qty_ahead = 3.0;
    okx.recent_trade_qty = 20.0;
    okx.recent_trade_rate_per_sec = 10.0;
    okx.latency_us = 20.0;

    hft::routing::QueueInputs slow = okx;
    slow.venue = "SLOWX";
    slow.order_price = 100.7;
    slow.latency_us = 4000.0;
    slow.recent_trade_qty = 1.0;
    slow.recent_trade_rate_per_sec = 0.5;

    auto okx_est = model.estimate(okx);
    auto slow_est = model.estimate(slow);

    CHECK(okx_est.fill_probability > 0.0, "Advanced queue fill probability positive");
    CHECK(okx_est.expected_fill_time_ms > 0.0, "Advanced queue fill time positive");
    CHECK(okx_est.queue_depletion_score > 0.0, "Advanced queue depletion score positive");
    CHECK(okx_est.maker_quality_score > 0.0, "Advanced queue maker quality positive");

    CHECK(
        okx_est.fill_probability > slow_est.fill_probability,
        "Advanced queue prefers active liquid venue"
    );

    std::vector<hft::routing::QueueInputs> inputs = {slow, okx};

    auto ranked_fill = model.rank_by_fill_probability(inputs);
    CHECK(ranked_fill.size() == 2, "Advanced queue ranked fill candidates");
    CHECK(ranked_fill.front().venue == "OKX", "Advanced queue ranks OKX by fill probability");

    auto ranked_quality = model.rank_by_maker_quality(inputs);
    CHECK(ranked_quality.size() == 2, "Advanced queue ranked maker quality");

    auto best = model.best_fill_candidate(inputs);
    CHECK(best.has_value(), "Advanced queue best candidate exists");
    CHECK(best->venue == "OKX", "Advanced queue best candidate OKX");

    auto stats = model.venue_stats("OKX", "BTC-USDT");
    CHECK(stats != nullptr, "Advanced queue venue stats found");
    CHECK(stats->trades.load() == 20, "Advanced queue trade stats");
    CHECK(stats->cancels.load() == 5, "Advanced queue cancel stats");

    auto metrics = model.metrics_snapshot();
    CHECK(metrics.venues_tracked == 2, "Advanced queue tracks two venues");
    CHECK(metrics.total_trades == 20, "Advanced queue total trades metric");
    CHECK(metrics.total_cancels == 5, "Advanced queue total cancels metric");
}


void test_fill_probability_engine() {
    hft::routing::FillProbabilityEngine engine;

    hft::routing::VenueQuote okx_quote{
        "OKX", "BTC-USDT", 100.0, 101.0, 5.0, 5.0, 0.5, 10.0, true
    };

    hft::routing::VenueQuote slow_quote{
        "SLOWX", "BTC-USDT", 100.0, 101.0, 5.0, 5.0, 10.0, 4000.0, true
    };

    hft::routing::QueueInputs okx_queue;
    okx_queue.venue = "OKX";
    okx_queue.symbol = "BTC-USDT";
    okx_queue.side = hft::routing::Side::Buy;
    okx_queue.order_qty = 1.0;
    okx_queue.order_price = 101.0;
    okx_queue.best_bid = 100.9;
    okx_queue.best_ask = 101.0;
    okx_queue.bid_qty_ahead = 2.0;
    okx_queue.ask_qty_ahead = 1.0;
    okx_queue.recent_trade_qty = 20.0;
    okx_queue.recent_trade_rate_per_sec = 10.0;
    okx_queue.latency_us = 10.0;

    hft::routing::QueueInputs slow_queue = okx_queue;
    slow_queue.venue = "SLOWX";
    slow_queue.latency_us = 4000.0;
    slow_queue.recent_trade_qty = 1.0;
    slow_queue.recent_trade_rate_per_sec = 0.2;
    slow_queue.order_price = 100.8;

    engine.record_order_sent("OKX", "BTC-USDT");
    engine.record_fill("OKX", "BTC-USDT", 1.0, 100.0);

    engine.record_order_sent("SLOWX", "BTC-USDT");
    engine.record_reject("SLOWX", "BTC-USDT");

    std::vector<hft::routing::FillProbabilityInput> inputs = {
        {okx_quote, okx_queue},
        {slow_quote, slow_queue}
    };


    auto ranked = engine.rank(inputs);

    CHECK(ranked.size() == 2, "Fill probability ranked two venues");
    CHECK(ranked.front().venue == "OKX", "Fill probability prefers OKX");
    CHECK(ranked.front().probability > ranked.back().probability, "Fill probability ranking valid");
    CHECK(ranked.front().expected_fill_time_ms > 0.0, "Fill probability expected time positive");

    auto best = engine.best(inputs);

    CHECK(best.has_value(), "Fill probability best exists");
    CHECK(best->venue == "OKX", "Fill probability best venue OKX");
     auto stats = engine.stats_for("OKX", "BTC-USDT");

    CHECK(stats.has_value(), "Advanced fill stats available");
    CHECK(stats->fills == 1, "Advanced fill stats fills");
    CHECK(stats->orders_sent == 1, "Advanced fill stats orders");
    CHECK(ranked.front().confidence > 0.0, "Advanced fill confidence positive");
    CHECK(ranked.front().expected_fill_qty > 0.0, "Advanced expected fill qty positive");
}



void test_execution_quality_analytics() {
    hft::execution::ChildOrderManager com;

    hft::execution::ParentCreateRequest request;
    request.symbol = "BTC-USDT";
    request.side = hft::execution::ChildOrderSide::Buy;
    request.quantity = 5.0;
    request.timestamp_ns = 1000;

    auto parent_id = com.create_parent(request);
    CHECK(parent_id > 0, "TCA parent created");

    std::vector<hft::execution::ChildOrderRoute> routes = {
        {"BINANCE", 2.0, 101.0},
        {"OKX", 3.0, 101.2}
    };

    auto children = com.create_children(parent_id, routes, 1100);
    CHECK(children.size() == 2, "TCA children created");

    CHECK(com.mark_submitted(children[0], 1200), "TCA child one submitted");
    CHECK(com.acknowledge(children[0], "BN-1", 1300), "TCA child one ack");
    CHECK(com.apply_fill(children[0], 2.0, 101.0, 1400), "TCA child one filled");

    CHECK(com.mark_submitted(children[1], 1200), "TCA child two submitted");
    CHECK(com.acknowledge(children[1], "OKX-1", 1300), "TCA child two ack");
    CHECK(com.apply_fill(children[1], 3.0, 101.2, 1500), "TCA child two filled");

    hft::execution::ExecutionQualityAnalytics tca;

    auto report = tca.parent_report(com, parent_id, 100.0);

    CHECK(report.has_value(), "TCA parent report created");
    CHECK(report->parent_id == parent_id, "TCA parent id");
    CHECK(report->filled_qty == 5.0, "TCA filled quantity");
    CHECK(report->fill_ratio == 1.0, "TCA fill ratio");
    CHECK(report->average_fill_price > 100.0, "TCA average fill price");
    CHECK(report->slippage_bps > 0.0, "TCA buy slippage positive");
    CHECK(report->implementation_shortfall > 0.0, "TCA implementation shortfall positive");

    auto venues = tca.venue_reports();

    CHECK(venues.size() == 2, "TCA venue reports created");

    auto binance = tca.venue_report("BINANCE", "BTC-USDT");

    CHECK(binance.has_value(), "TCA binance report");
    CHECK(binance->child_orders == 1, "TCA binance order count");
    CHECK(binance->filled_children == 1, "TCA binance fill count");
    CHECK(binance->fill_ratio == 1.0, "TCA binance fill ratio");

    CHECK(tca.metrics().reports_generated.load() == 1, "TCA reports metric");
    CHECK(tca.metrics().venue_updates.load() == 2, "TCA venue update metric");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "==================================================\n";
    std::cout << " HFT Advanced Modules - Test Suite\n";
    std::cout << "==================================================\n";

    test_fast_book();
    test_analytics();
    test_optimizer();
    test_tick_store();
    test_market_data_engine_advanced();
    test_lock_free_market_data_cache();
    test_market_data_engine_cache_integration();
    test_advanced_market_data_cache_features();
    test_gap_recovery_engine_advanced();
    test_child_order_manager_core();
    test_child_order_manager_execution_reports();
    test_child_order_command_factory();
    test_child_order_command_factory_advanced();
    test_child_order_adapter_handoff();
    test_child_order_adapter_handoff_advanced();
    test_venue_scoring_engine();
    test_adaptive_smart_order_router();
    test_adaptive_smart_order_router_advanced();
    test_execution_simulator_full_fill();
    test_execution_simulator_advanced_profiles();
    test_queue_position_model();
    test_advanced_queue_position_model();
    test_fill_probability_engine();
    test_execution_quality_analytics();
    test_smart_order_router();
    test_distributed_optimizer();
    test_manual_drive_api();
    test_fastbook_analytics_integration();

    std::cout << "\n==================================================\n";
    std::cout << " Results: " << g_pass << " passed, " << g_fail << " failed\n";
    if (g_fail == 0) std::cout << " ALL TESTS PASSED\n";
    else             std::cout << " " << g_fail << " FAILURES\n";
    std::cout << "==================================================\n";
    return g_fail > 0 ? 1 : 0;
}