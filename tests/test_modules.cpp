// ─────────────────────────────────────────────────────────────────────────────
// tests/test_modules.cpp  —  Tests for the 6 new production modules
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/hft.hpp"
#include "oms_reconciler.hpp"
#include <iostream>
#include <iomanip>
#include <cassert>
#include <cmath>
#include <chrono>
#include <sstream>
#include <thread>

using namespace hft;

// ─── Utilities ────────────────────────────────────────────────────────────────
static int g_pass = 0, g_fail = 0;
#define SUITE(n) std::cout << "\n=== " << n << " ===\n"
#define CHECK(expr, msg) do { \
    if (!(expr)) { ++g_fail; std::cerr << "  FAIL [" << __LINE__ << "] " << (msg) << "\n"; } \
    else { ++g_pass; std::cout << "  PASS: " << (msg) << "\n"; } \
} while(0)
#define CHECK_NEAR(a, b, tol, msg) CHECK(std::abs((double)(a)-(double)(b)) < (tol), msg)

// Shared synthetic data generator
static std::vector<MarketEvent> make_data(
    const std::string& sym, double price, int64_t dur_s, uint64_t seed = 42)
{
    AssetParams ap;
    ap.symbol = sym; ap.initial_price = price;
    ap.annual_vol = 0.75; ap.tick_size = 0.01;
    ap.depth_levels = 8; ap.trade_intensity = 6.0;
    ap.hawkes_alpha = 0.3; ap.seed = seed;
    return SyntheticGenerator(ap).generate(static_cast<double>(dur_s), 5000);
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. InventoryManager
// ─────────────────────────────────────────────────────────────────────────────
void test_inventory_manager() {
    SUITE("InventoryManager");

    InventoryManagerConfig cfg;
    cfg.reference_symbol = "BTCUSDT";
    cfg.target_beta_delta = 0.0;
    cfg.hedge_threshold_usd = 5'000.0;
    cfg.max_gross_notional = 50'000.0;
    cfg.max_concentration_hhi = 1.0;
    cfg.inventory_halflife_s = 300.0;

    InventoryManager inv(cfg);

    InventoryRiskLimit btc_lim;
    btc_lim.max_qty = 1.0;
    btc_lim.max_notional = 100'000.0;
    btc_lim.max_beta_delta = 100'000.0;

    inv.register_asset("BTCUSDT", 1.0, 1.0, btc_lim);
    inv.register_asset("ETHUSDT", 1.0, 0.80);

    auto make_fill = [](const std::string& sym, Side s, double qty, double price, int64_t ts) {
        FillEvent f;
        f.order_id = "x";
        f.symbol = sym;
        f.side = s;
        f.qty = qty;
        f.price = price;
        f.timestamp = ts;
        f.realized_pnl = 0.0;
        f.fee = 0.0;
        return f;
    };

    inv.on_fill(make_fill("BTCUSDT", Side::Buy, 0.1, 43500.0, 1'000'000'000LL));
    CHECK_NEAR(inv.qty("BTCUSDT"), 0.1, 1e-9, "BTC position = +0.1 after buy");

    const auto* btc = inv.get("BTCUSDT");
    CHECK(btc != nullptr, "get() returns non-null for registered asset");
    CHECK_NEAR(btc->avg_cost, 43500.0, 1.0, "avg_cost = 43500 after single buy");

    inv.on_fill(make_fill("BTCUSDT", Side::Buy, 0.1, 44000.0, 2'000'000'000LL));
    double expected_avg = (0.1 * 43500.0 + 0.1 * 44000.0) / 0.2;
    CHECK_NEAR(inv.get("BTCUSDT")->avg_cost, expected_avg, 1.0, "Weighted avg cost after 2 buys");
    CHECK_NEAR(inv.qty("BTCUSDT"), 0.2, 1e-9, "BTC position = +0.2");

    inv.on_fill(make_fill("BTCUSDT", Side::Sell, 0.1, 44500.0, 3'000'000'000LL));
    CHECK_NEAR(inv.qty("BTCUSDT"), 0.1, 1e-9, "BTC position = +0.1 after partial sell");

    std::unordered_map<std::string,double> mids = {
        {"BTCUSDT", 44800.0},
        {"ETHUSDT", 2300.0}
    };

    auto snap = inv.snapshot(mids);
    CHECK(snap.gross_notional > 0.0, "Gross notional positive");
    CHECK(snap.beta_weighted_delta > 0.0, "Beta-weighted delta positive");
    CHECK(std::isfinite(snap.unrealized_pnl), "Unrealized PnL finite");

    double upnl = inv.get("BTCUSDT")->unrealized_pnl(44800.0);
    CHECK(upnl > 0.0, "Unrealized PnL positive");

    auto hedges = inv.hedge_recommendations(mids);
    CHECK(hedges.empty(), "No hedge needed below threshold");

    inv.on_fill(make_fill("BTCUSDT", Side::Buy, 0.2, 44000.0, 4'000'000'000LL));
    auto hedges2 = inv.hedge_recommendations(mids);
    CHECK(!hedges2.empty(), "Hedge triggered above threshold");
    CHECK(hedges2[0].side == Side::Sell, "Hedge is sell to reduce long delta");

    double decay_trade = inv.inventory_decay_trade("BTCUSDT", 300.0);
    CHECK(decay_trade < 0.0, "Decay trade is negative for long position");

    inv.on_fill(make_fill("ETHUSDT", Side::Buy, 1.0, 2300.0, 5'000'000'000LL));
    auto snap2 = inv.snapshot(mids);
    CHECK(snap2.beta_weighted_delta > 0.0, "Beta-weighted delta positive when long both assets");

    auto breaches = inv.check_limits(mids);
    CHECK(breaches.empty(), "No inventory limit breaches");

    auto stress = inv.stress_test(mids);
    CHECK(!stress.empty(), "Stress test returns scenarios");

    double funding = inv.funding_pnl_estimate(mids, 1.0);
    CHECK(std::isfinite(funding), "Funding estimate finite");

    double vel = inv.position_velocity("BTCUSDT");
    CHECK(std::isfinite(vel), "Position velocity finite");

    std::cout << "  BTC qty       : " << std::setprecision(4) << inv.qty("BTCUSDT") << "\n";
    std::cout << "  Gross notional: $" << std::setprecision(2) << snap2.gross_notional << "\n";
    std::cout << "  Beta delta    : $" << snap2.beta_weighted_delta << "\n";
    std::cout << "  Unreal PnL    : $" << snap2.unrealized_pnl << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. RiskManager
// ─────────────────────────────────────────────────────────────────────────────
void test_risk_manager() {
    SUITE("RiskManager");

    RiskManagerConfig cfg;
    cfg.max_order_notional_usd  = 10'000.0;
    cfg.max_price_deviation_bps = 30.0;
    cfg.max_orders_per_second   = 5;
    cfg.max_daily_notional_usd  = 100'000.0;
    cfg.max_position_qty["BTCUSDT"] = 0.5;
    cfg.available_margin_usd    = 100'000.0;
    cfg.margin_rate             = 0.10;
    cfg.max_daily_loss_usd      = 1'000.0;
    cfg.max_drawdown_pct        = 0.10;
    cfg.max_consecutive_losses  = 5;
    cfg.halt_on_circuit_breaker = true;

    std::vector<RiskViolation> violations_caught;
    cfg.on_violation = [&](const RiskViolation& v) {
        violations_caught.push_back(v);
    };

    RiskManager rm(cfg);

    int64_t ts = 1'000'000'000LL;

    // Build a minimal order book for price sanity checks
    OrderBook book("BTCUSDT", 0.01);
    book.apply_l2({.symbol="BTCUSDT",.side=BookSide::Bid,.price=43500.0,.qty=1.0,.timestamp=ts});
    book.apply_l2({.symbol="BTCUSDT",.side=BookSide::Ask,.price=43501.0,.qty=1.0,.timestamp=ts});

    // ── Pre-trade checks ──────────────────────────────────────────────────────

    // Normal order: should pass
    auto v1 = rm.check_order("BTCUSDT", Side::Buy, 43501.0, 0.1, ts, &book);
    CHECK(v1.empty(), "Normal order passes all checks");

    // Fat finger: qty × price = 43501 × 1.0 = $43501 > $10000 limit
    auto v2 = rm.check_order("BTCUSDT", Side::Buy, 43501.0, 1.0, ts, &book);
    bool has_fat = false;
    for (auto& v : v2) if (v.kind == ViolationKind::FatFinger) has_fat = true;
    CHECK(has_fat, "Fat-finger violation detected for large notional");

    // Price sanity: price 45000 is > 30bps from mid 43500.5
    auto v3 = rm.check_order("BTCUSDT", Side::Buy, 45000.0, 0.1, ts, &book);
    bool has_ps = false;
    for (auto& v : v3) if (v.kind == ViolationKind::PriceSanity) has_ps = true;
    CHECK(has_ps, "Price sanity violation for price far from mid");

    // Rate limit: send 6 orders in 1 second
    for (int i = 0; i < 5; ++i) {
        rm.on_order_sent("BTCUSDT", 43501.0, 0.1, ts + i * 100'000'000LL);
    }
    auto v4 = rm.check_order("BTCUSDT", Side::Buy, 43501.0, 0.1, ts + 500'000'000LL, &book);
    bool has_rate = false;
    for (auto& v : v4) if (v.kind == ViolationKind::OrderRateLimit) has_rate = true;
    CHECK(has_rate, "Rate limit violation after 5 orders/sec");

    // ── Post-trade circuit breakers ───────────────────────────────────────────
    RiskManager rm2(cfg);

    // Trigger daily loss limit
    FillEvent f;
    f.order_id="x"; f.symbol="BTCUSDT"; f.side=Side::Sell;
    f.qty=0.1; f.price=43000.0; f.timestamp=ts;
    f.realized_pnl = -1500.0;  // big loss
    f.fee = 0.0;

    auto cb1 = rm2.on_fill(f, 48500.0, ts);
    bool has_daily = false;
    for (auto& v : cb1) if (v.kind == ViolationKind::DailyLossLimit) has_daily = true;
    CHECK(has_daily, "Daily loss circuit breaker triggered");
    CHECK(rm2.is_halted(), "System halted after daily loss breach");

    // Verify violation callback was called
    RiskManager rm3(cfg);
    violations_caught.clear();
    rm3.on_fill(f, 48500.0, ts);
    CHECK(!violations_caught.empty(), "Violation callback fired on circuit breaker");

    // Consecutive losses
    RiskManager rm4(cfg);
    FillEvent loss;
    loss.order_id="x"; loss.symbol="BTCUSDT"; loss.side=Side::Sell;
    loss.qty=0.01; loss.price=43000.0; loss.timestamp=ts;
    loss.realized_pnl = -10.0; loss.fee = 0.0;
    for (int i = 0; i < 4; ++i) rm4.on_fill(loss, 50000.0, ts + i*1e9);
    CHECK(!rm4.is_halted(), "Not halted after 4 consecutive losses (limit=5)");
    rm4.on_fill(loss, 50000.0, ts + 5e9);
    CHECK(rm4.is_halted(), "Halted after 5 consecutive losses");

    // Manual halt/resume
    RiskManager rm5(cfg);
    rm5.manual_halt();
    CHECK(rm5.is_halted(), "Manual halt works");
    auto v5 = rm5.check_order("BTCUSDT", Side::Buy, 43501.0, 0.1, ts);
    bool has_halt = false;
    for (auto& v : v5) if (v.kind == ViolationKind::ManualHalt) has_halt = true;
    CHECK(has_halt, "ManualHalt violation returned when halted");
    rm5.manual_resume();
    CHECK(!rm5.is_halted(), "Manual resume works");

    // Reset daily
    RiskManager rm6(cfg);
    rm6.on_order_sent("BTCUSDT", 43501.0, 0.1, ts);
    CHECK(rm6.orders_last_second(ts + 100'000'000LL) == 1, "1 order in last second");
    rm6.reset_daily();
    CHECK(rm6.daily_pnl() == 0.0, "Daily PnL reset to 0");

    // Summary
    auto sum = rm.summary();
    CHECK(!sum.halted, "rm summary: not halted");
    std::cout << "  Daily notional: $" << std::setprecision(2) << sum.daily_notional << "\n";
    std::cout << "  Used margin   : $" << sum.used_margin << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Profiler
// ─────────────────────────────────────────────────────────────────────────────
void test_profiler() {
    SUITE("Profiler");

    Profiler prof;

    // Scoped timer
    {
        auto t = prof.scoped("section_a");
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    {
        auto t = prof.scoped("section_b");
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    for (int i = 0; i < 10; ++i) {
        auto t = prof.scoped("section_a");
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    // Manual start/stop
    auto t0 = prof.start("manual");
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    prof.stop("manual", t0);

    CHECK(prof.section_count() == 3, "3 sections registered");

    auto* sa = prof.get("section_a");
    CHECK(sa != nullptr, "section_a exists");
    CHECK(sa->count == 11, "section_a called 11 times");
    CHECK(sa->mean_ns() > 0.0, "section_a mean_ns > 0");
    CHECK(sa->p99_ns() >= sa->mean_ns(), "p99 >= mean");
    CHECK(sa->max_ns >= sa->min_ns, "max >= min");

    auto* sb = prof.get("section_b");
    CHECK(sb && sb->count == 1, "section_b called once");

    auto* sm = prof.get("manual");
    CHECK(sm && sm->count == 1, "manual section called once");
    CHECK(sm->mean_ns() > 0.0, "manual section recorded positive mean duration");

    // CSV export
    std::string csv = prof.to_csv();
    CHECK(!csv.empty(), "to_csv non-empty");
    CHECK(csv.find("section_a") != std::string::npos, "CSV contains section_a");
    CHECK(csv.find("mean_ns") != std::string::npos, "CSV has header");

    // Reset
    prof.reset("section_b");
    CHECK(prof.get("section_b") == nullptr, "section_b removed after reset");
    prof.reset_all();
    CHECK(prof.section_count() == 0, "All sections cleared after reset_all");

    // ProfiledStrategy wrapper
    {
        class NullStrat : public Strategy {};
        NullStrat null_strat;
        ProfiledStrategy ps(null_strat);

        // Create a minimal engine just to test the wrapper compiles
        SimEngine eng(ps, binance_colocation(), FillModelConfig{}, 1000.0);
        eng.add_symbol("BTCUSDT", 0.01);

        // Feed one event
        L2Update u; u.symbol="BTCUSDT"; u.side=BookSide::Bid;
        u.price=43500.0; u.qty=1.0; u.timestamp=1'000'000'000LL;
        auto evts = std::vector<MarketEvent>{u};
        eng.run(evts);

        auto& pp = ps.profiler();
        (void)pp;
        CHECK(true, "ProfiledStrategy profiler accessible");
    }

    std::cout << "  section_a: count=" << prof.get("section_a") << " (after reset_all)\n";

    // ReservoirSampler
    {
        ReservoirSampler rs;
        for (int i = 1; i <= 10000; ++i) rs.add(i);
        double p50 = rs.percentile(50);
        double p99 = rs.percentile(99);
        CHECK(p50 > 4000 && p50 < 6000, "Reservoir p50 ≈ 5000");
        CHECK(p99 > 9000, "Reservoir p99 > 9000");
        CHECK(rs.total() == 10000, "Reservoir total == 10000");
        std::cout << "  Reservoir p50=" << std::setprecision(0) << p50
                  << " p99=" << p99 << "\n";
    }
}



void test_portfolio_risk_engine() {
    SUITE("PortfolioRiskEngine");

    RiskLimits base_limits;
    PortfolioState portfolio(100000.0, base_limits);

    FillEvent buy_btc;
    buy_btc.order_id = "f1";
    buy_btc.symbol = "BTCUSDT";
    buy_btc.side = Side::Buy;
    buy_btc.price = 50000.0;
    buy_btc.qty = 0.5;
    buy_btc.timestamp = 1000;
    portfolio.update_on_fill(buy_btc);

    FillEvent buy_eth;
    buy_eth.order_id = "f2";
    buy_eth.symbol = "ETHUSDT";
    buy_eth.side = Side::Buy;
    buy_eth.price = 2500.0;
    buy_eth.qty = 5.0;
    buy_eth.timestamp = 2000;
    portfolio.update_on_fill(buy_eth);

    std::unordered_map<std::string, double> prices;
    prices["BTCUSDT"] = 51000.0;
    prices["ETHUSDT"] = 2600.0;

    portfolio.snapshot(3000, prices);

    PortfolioRiskLimits limits;
    limits.max_gross_exposure = 1000000.0;
    limits.max_net_exposure = 1000000.0;
    limits.max_leverage = 5.0;
    limits.max_var_95 = 100000.0;
    limits.max_cvar_95 = 150000.0;
    limits.max_mc_var_99 = 200000.0;
    limits.max_margin_usage = 0.90;
    limits.max_concentration = 0.80;
    limits.max_drawdown = 0.30;
    limits.max_liquidity_days = 10.0;
    limits.max_stress_loss_frac = 0.30;

    PortfolioRiskEngine engine(limits, 42);

    engine.set_asset_input({"BTCUSDT", 51000.0, 0.04, 1000000000.0, 0.10, 0.10});
    engine.set_asset_input({"ETHUSDT", 2600.0, 0.05, 800000000.0, 0.12, 0.10});
    engine.set_correlation("BTCUSDT", "ETHUSDT", 0.75);

    engine.add_scenario({
        "crypto_crash",
        {
            {"BTCUSDT", -0.20},
            {"ETHUSDT", -0.30}
        }
    });

    engine.add_factor({
        "crypto_beta",
        {
            {"BTCUSDT", 1.0},
            {"ETHUSDT", 1.2}
        }
    });

    auto report = engine.evaluate(portfolio, prices, 1000);

    CHECK(report.equity > 0.0, "Risk report equity positive");
    CHECK(report.gross_exposure > 0.0, "Gross exposure positive");
    CHECK(report.net_exposure > 0.0, "Net exposure positive");
    CHECK(report.leverage >= 0.0, "Leverage non-negative");

    CHECK(report.parametric_var_95 > 0.0, "Parametric VaR positive");
    CHECK(report.parametric_cvar_95 >= report.parametric_var_95, "CVaR >= VaR");
    CHECK(report.monte_carlo_var_99 >= 0.0, "Monte Carlo VaR non-negative");

    CHECK(report.margin_required > 0.0, "Margin required positive");
    CHECK(report.margin_usage >= 0.0, "Margin usage non-negative");

    CHECK(report.max_concentration > 0.0, "Concentration computed");
    CHECK(!report.max_concentration_symbol.empty(), "Concentration symbol set");

    CHECK(report.liquidity_days >= 0.0, "Liquidity days non-negative");
    CHECK(report.worst_stress_loss > 0.0, "Stress loss positive");
    CHECK(report.worst_stress_name == "crypto_crash", "Worst stress scenario selected");

    CHECK(report.factor_exposures.count("crypto_beta") == 1, "Factor exposure computed");
    CHECK(!report.contributions.empty(), "Risk contributions computed");

    std::cout << "  Equity      : $" << report.equity << "\n";
    std::cout << "  Gross exp   : $" << report.gross_exposure << "\n";
    std::cout << "  VaR 95      : $" << report.parametric_var_95 << "\n";
    std::cout << "  MC VaR 99   : $" << report.monte_carlo_var_99 << "\n";
    std::cout << "  Stress loss : $" << report.worst_stress_loss << "\n";
}



// ─────────────────────────────────────────────────────────────────────────────
// 4. Config system
// ─────────────────────────────────────────────────────────────────────────────
void test_config() {
    SUITE("Config");

    // Load from string
    Config cfg;
    cfg.load_string(R"(
# HFT config test
[strategy]
symbol       = BTCUSDT
as_gamma     = 0.15
as_k         = 1.8
min_spread   = 2.0
use_glft     = true
vol_halflife = 300

[risk]
max_drawdown      = 0.08
max_daily_loss    = 0.02
halt_on_breach    = true
max_pos_BTCUSDT   = 0.20
max_pos_ETHUSDT   = 2.00

[fill]
maker_fee    = -0.0002
taker_fee    = 0.0004
fill_mode    = fifo

[latency]
preset       = binance_colo

[portfolio]
initial_cash = 75000.0
)");

    // Key access
    CHECK(cfg.has("strategy.as_gamma"),        "has() finds strategy.as_gamma");
    CHECK(!cfg.has("nonexistent.key"),      "has() returns false for missing key");
    CHECK_NEAR(cfg.get_double("strategy.as_gamma", 0.0), 0.15, 1e-9,
               "get_double: as_gamma = 0.15 (as_gamma key)");
    CHECK(cfg.get_int("strategy.vol_halflife", 0) == 300,
          "get_int: vol_halflife = 300");
    CHECK(cfg.get_bool("strategy.use_glft", false),
          "get_bool: use_glft = true");
    CHECK(cfg.get_string("strategy.symbol", "") == "BTCUSDT",
          "get_string: symbol = BTCUSDT");
    CHECK_NEAR(cfg.get_double("portfolio.initial_cash", 0.0), 75000.0, 1.0,
               "get_double: initial_cash = 75000");

    // Default values
    CHECK_NEAR(cfg.get_double("missing.key", 42.0), 42.0, 1e-9,
               "Missing key returns default");
    CHECK(cfg.get_string("missing.key", "default") == "default",
          "Missing string returns default");

    // Struct builders
    auto acfg = cfg.to_asset_config("strategy");
    CHECK(acfg.symbol == "BTCUSDT",       "to_asset_config: symbol correct");
    CHECK_NEAR(acfg.as_gamma, 0.15, 1e-9, "to_asset_config: gamma correct");
    CHECK_NEAR(acfg.as_k,     1.8,  1e-9, "to_asset_config: k correct");
    CHECK(acfg.use_glft,                   "to_asset_config: use_glft = true");
    CHECK(acfg.vol_halflife == 300,        "to_asset_config: vol_halflife = 300");

    auto fcfg = cfg.to_fill_config("fill");
    CHECK_NEAR(fcfg.maker_fee, -0.0002, 1e-9, "to_fill_config: maker_fee correct");
    CHECK(fcfg.fill_mode == FillMode::FIFO,   "to_fill_config: fill_mode = FIFO");

    auto rlim = cfg.to_risk_limits("risk");
    CHECK_NEAR(rlim.max_drawdown,  0.08, 1e-9, "to_risk_limits: max_drawdown");
    CHECK_NEAR(rlim.max_daily_loss,0.02, 1e-9, "to_risk_limits: max_daily_loss");
    CHECK(rlim.max_position.count("BTCUSDT"), "to_risk_limits: BTCUSDT position limit");
    CHECK_NEAR(rlim.max_position["BTCUSDT"], 0.20, 1e-9,
               "to_risk_limits: BTCUSDT limit = 0.20");
    CHECK(rlim.max_position.count("ETHUSDT"), "to_risk_limits: ETHUSDT position limit");

    auto lat = cfg.to_latency_profile("latency");
    CHECK(lat.feed_base > 0, "to_latency_profile: preset loaded");

    // required() validation
    bool threw = false;
    try { cfg.required({"strategy.as_gamma", "nonexistent.field"}); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw, "required() throws on missing key");

    cfg.required({"strategy.as_gamma", "strategy.symbol"});  // should not throw
    CHECK(true, "required() passes when all keys present");

    // Setters
    cfg.set_double("new.value", 3.14);
    CHECK_NEAR(cfg.get_double("new.value"), 3.14, 1e-9, "set_double works");
    cfg.set_bool("new.flag", true);
    CHECK(cfg.get_bool("new.flag"), "set_bool works");
    cfg.set("new.str", "hello");
    CHECK(cfg.get_string("new.str") == "hello", "set() works");

    // Double list
    cfg.load_string("[data]\ncorrelations = 1.0, 0.85, 0.85, 1.0\n");
    auto corr = cfg.get_double_list("data.correlations");
    CHECK(corr.size() == 4, "get_double_list: 4 values");
    CHECK_NEAR(corr[1], 0.85, 1e-9, "get_double_list: second value correct");

    // Dump
    std::ostringstream oss;
    cfg.dump(oss);
    CHECK(!oss.str().empty(), "dump() produces non-empty output");

    // Default config string
    std::string def = default_config_string();
    CHECK(!def.empty(), "default_config_string non-empty");
    Config def_cfg;
    def_cfg.load_string(def);
    CHECK(def_cfg.has("strategy.as_gamma"),  "Default config has strategy.as_gamma");
    CHECK(def_cfg.has("latency.preset"),     "Default config has latency.preset");
    CHECK(def_cfg.has("risk.max_drawdown"),  "Default config has risk.max_drawdown");
    std::cout << "  Config size: " << cfg.size() << " keys\n";
}


// ─────────────────────────────────────────────────────────────────────────────
// 5. StatArb pairs strategy
// ─────────────────────────────────────────────────────────────────────────────
void test_stat_arb() {
    SUITE("StatArb Pairs Strategy");

    // Generate correlated BTC + ETH data
    AssetParams btc_p, eth_p;
    btc_p.symbol="BTCUSDT"; btc_p.initial_price=43500.0;
    btc_p.annual_vol=0.75; btc_p.tick_size=0.01;
    btc_p.depth_levels=8; btc_p.trade_intensity=6.0; btc_p.seed=42;
    eth_p.symbol="ETHUSDT"; eth_p.initial_price=2250.0;
    eth_p.annual_vol=0.85; eth_p.tick_size=0.01;
    eth_p.depth_levels=8; eth_p.trade_intensity=8.0; eth_p.seed=43;

    std::vector<double> corr = {1.0, 0.85, 0.85, 1.0};
    CorrelatedGenerator cgen({btc_p, eth_p}, corr);
    auto events = cgen.generate(30.0, 5000);   // 30 seconds
    CHECK(!events.empty(), "Correlated events generated");

    // Configure StatArb
    PairsConfig pairs_cfg;
    pairs_cfg.leg_a       = "BTCUSDT";
    pairs_cfg.leg_b       = "ETHUSDT";
    pairs_cfg.tick_a      = 0.01;
    pairs_cfg.tick_b      = 0.01;
    pairs_cfg.qty_a       = 0.01;
    pairs_cfg.ols_window  = 50;
    pairs_cfg.entry_z     = 1.5;
    pairs_cfg.exit_z      = 0.3;
    pairs_cfg.stop_z      = 3.5;
    pairs_cfg.ewma_hl     = 30;
    pairs_cfg.max_position_a = 0.1;
    pairs_cfg.min_pnl_bps = 2.0;

    StatArbStrategy strat(pairs_cfg);

    FillModelConfig fcfg;
    fcfg.maker_fee = -0.0002; fcfg.taker_fee = 0.0004;

    SimEngine engine(strat, binance_colocation(), fcfg, 50'000.0);
    engine.add_symbol("BTCUSDT", 0.01);
    engine.add_symbol("ETHUSDT", 0.01);

    auto stats = engine.run(events);

    CHECK(stats.strategy_errors == 0, "No strategy errors in StatArb");

    // After enough ticks, beta should have been estimated
    double beta = strat.beta();
    CHECK(beta > 0.0 && beta < 20.0, "OLS beta estimated and reasonable");
    std::cout << "  OLS beta       : " << std::setprecision(4) << beta << "\n";
    std::cout << "  Z-score        : " << strat.z_score() << "\n";
    std::cout << "  Entries        : " << strat.state().entries << "\n";
    std::cout << "  Exits          : " << strat.state().exits << "\n";
    std::cout << "  Stops          : " << strat.state().stops << "\n";
    std::cout << "  Total fills    : " << stats.total_fills << "\n";
    std::cout << "  P&L            : $" << std::setprecision(4)
              << stats.portfolio_summary.pnl << "\n";

    // State accessors
    const auto& st = strat.state();
    CHECK(std::isfinite(st.z_score), "Z-score is finite");
    CHECK(std::isfinite(st.beta),    "Beta is finite");
    CHECK(std::isfinite(st.mu_spread),"Kalman mu_spread is finite");
    CHECK(st.sigma_spread > 0.0,     "EWMA sigma_spread > 0");
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Execution algorithms
// ─────────────────────────────────────────────────────────────────────────────
void test_execution_algos() {
    SUITE("Execution Algorithms (TWAP, Iceberg, POV)");

    auto events = make_data("BTCUSDT", 43500.0, 30, 77);
    CHECK(!events.empty(), "Test data generated");

    // ── TWAP ────────────────────────────────────────────────────────────────
    {
        int64_t start_ns = 1'700'000'000'000'000'000LL;
        int64_t end_ns   = start_ns + 30'000'000'000LL;  // 30 seconds

        ParentOrder parent;
        parent.order_id   = "parent_twap";
        parent.symbol     = "BTCUSDT";
        parent.side       = Side::Buy;
        parent.total_qty  = 0.10;   // Buy 0.1 BTC over 30 seconds
        parent.limit_price= 0.0;    // no limit
        parent.start_ns   = start_ns;
        parent.end_ns     = end_ns;

        TwapConfig tcfg;
        tcfg.n_slices         = 10;
        tcfg.max_participation= 0.50;
        tcfg.price_offset_bps = 1.0;
        tcfg.use_limit_orders = true;

        TwapExecutor twap(parent, tcfg);
        twap.set_tick_size(0.01);

        FillModelConfig fcfg; fcfg.maker_fee=-0.0002; fcfg.taker_fee=0.0004;
        SimEngine eng(twap, binance_colocation(), fcfg, 50'000.0);
        eng.add_symbol("BTCUSDT", 0.01);
        eng.run(events);

        CHECK(twap.slices_sent() >= 0, "TWAP: slices_sent non-negative");
        CHECK(twap.fill_pct() >= 0.0 && twap.fill_pct() <= 100.0,
              "TWAP: fill_pct in [0,100]");
        std::cout << "  TWAP slices sent: " << twap.slices_sent() << "\n";
        std::cout << "  TWAP fill %     : " << std::setprecision(1)
                  << twap.fill_pct() << "%\n";
    }

    // ── Iceberg ─────────────────────────────────────────────────────────────
    {
        int64_t start_ns = 1'700'000'000'000'000'000LL;
        ParentOrder parent;
        parent.order_id   = "parent_ice";
        parent.symbol     = "BTCUSDT";
        parent.side       = Side::Sell;
        parent.total_qty  = 0.08;
        parent.limit_price= 0.0;
        parent.start_ns   = start_ns;
        parent.end_ns     = start_ns + 60'000'000'000LL;

        IcebergConfig icfg;
        icfg.visible_qty      = 0.01;
        icfg.price_offset_bps = 1.0;
        icfg.randomize_qty    = false;
        icfg.randomize_price  = false;

        IcebergExecutor iceberg(parent, icfg);
        iceberg.set_tick_size(0.01);

        FillModelConfig fcfg; fcfg.maker_fee=-0.0002; fcfg.taker_fee=0.0004;
        SimEngine eng(iceberg, binance_colocation(), fcfg, 50'000.0);
        eng.add_symbol("BTCUSDT", 0.01);
        eng.run(events);

        CHECK(iceberg.fill_pct() >= 0.0, "Iceberg: fill_pct >= 0");
        CHECK(iceberg.slices_sent() >= 0, "Iceberg: slices_sent >= 0");
        std::cout << "  Iceberg slices  : " << iceberg.slices_sent() << "\n";
        std::cout << "  Iceberg fill %  : " << std::setprecision(1)
                  << iceberg.fill_pct() << "%\n";
    }

    // ── POV ─────────────────────────────────────────────────────────────────
    {
        int64_t start_ns = 1'700'000'000'000'000'000LL;
        ParentOrder parent;
        parent.order_id   = "parent_pov";
        parent.symbol     = "BTCUSDT";
        parent.side       = Side::Buy;
        parent.total_qty  = 0.05;
        parent.limit_price= 0.0;
        parent.start_ns   = start_ns;
        parent.end_ns     = start_ns + 60'000'000'000LL;

        POVConfig pcfg;
        pcfg.pov_rate         = 0.15;
        pcfg.price_offset_bps = 0.5;
        pcfg.min_child_qty    = 0.001;
        pcfg.max_child_qty    = 0.05;

        POVExecutor pov(parent, pcfg);
        pov.set_tick_size(0.01);

        FillModelConfig fcfg; fcfg.maker_fee=-0.0002; fcfg.taker_fee=0.0004;
        SimEngine eng(pov, binance_colocation(), fcfg, 50'000.0);
        eng.add_symbol("BTCUSDT", 0.01);
        eng.run(events);

        CHECK(pov.fill_pct() >= 0.0, "POV: fill_pct >= 0");
        std::cout << "  POV fill %      : " << std::setprecision(1)
                  << pov.fill_pct() << "%\n";
        const auto& pp = pov.parent();
        CHECK(pp.filled_qty >= 0.0, "POV: filled_qty >= 0");
        CHECK(pp.filled_qty <= pp.total_qty + 1e-9, "POV: filled_qty <= total_qty");
    }
}


void test_smart_router() {
    SUITE("SmartOrderRouter");

    OrderBook binance("BTCUSDT", 0.01);
    OrderBook bybit("BTCUSDT", 0.01);

    binance.apply_l2({"BTCUSDT", BookSide::Bid, 100.0, 5.0, 1, 1});
    binance.apply_l2({"BTCUSDT", BookSide::Ask, 101.0, 5.0, 2, 2});

    bybit.apply_l2({"BTCUSDT", BookSide::Bid, 100.2, 2.0, 1, 1});
    bybit.apply_l2({"BTCUSDT", BookSide::Ask, 100.8, 2.0, 2, 2});

    SmartOrderRouter router;

    router.add_venue({"binance"});
    router.add_venue({"bybit"});

    router.update_book("binance", "BTCUSDT", &binance);
    router.update_book("bybit", "BTCUSDT", &bybit);

    RouteRequest req;
    req.symbol = "BTCUSDT";
    req.side = Side::Buy;
    req.qty = 1.0;
    req.liquidity = LiquidityPreference::TakerOnly;
    req.style = RoutingStyle::BestVenue;
    req.allow_partial = false;

    auto decision = router.route(req);

    CHECK(decision.ok, "Route decision OK");
    CHECK(!decision.legs.empty(), "Route has legs");
    CHECK(decision.legs[0].venue == "bybit", "Best taker buy routes to cheaper ask venue");
    CHECK_NEAR(decision.legs[0].price, 100.8, 1e-9, "Route price is Bybit ask");

    req.qty = 10.0;
    req.allow_partial = true;
    req.style = RoutingStyle::SplitByLiquidity;

    auto split = router.route(req);
    CHECK(split.ok, "Split route OK");
    CHECK(split.routed_qty > 0.0, "Split routed some qty");
    CHECK(split.unfilled_qty >= 0.0, "Split reports unfilled qty");
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. Integration: Config → RiskManager → ProfiledStrategy → Analytics
// ─────────────────────────────────────────────────────────────────────────────
void test_full_integration() {
    SUITE("Full Integration: Config → Risk → Profiler → Analytics");

    // Load config from string
    Config cfg;
    cfg.load_string(R"(
[strategy]
symbol           = BTCUSDT
tick_size        = 0.01
quote_qty        = 0.01
max_inventory    = 0.10
as_gamma         = 0.12
as_k             = 1.5
as_T             = 300.0
min_spread_bps   = 1.5
max_spread_bps   = 35.0
use_glft         = true
toxicity_pause   = 0.85
max_quote_age_ms = 4000.0
min_refresh_us   = 400.0

[fill]
maker_fee        = -0.0002
taker_fee        = 0.0004
fill_mode        = fifo

[risk]
max_drawdown     = 0.10
max_daily_loss   = 0.03
halt_on_breach   = true
max_pos_BTCUSDT  = 0.20

[latency]
preset           = binance_colo

[portfolio]
initial_cash     = 50000.0
)");

    auto acfg  = cfg.to_asset_config("strategy", "BTCUSDT");
    auto fcfg  = cfg.to_fill_config("fill");
    auto rlim  = cfg.to_risk_limits("risk");
    auto lat   = cfg.to_latency_profile("latency");
    double cash= cfg.get_double("portfolio.initial_cash", 50000.0);

    // Build profiled strategy
    MultiAssetMarketMaker mm({acfg}, 0.0003);
    ProfiledStrategy ps(mm);

    // Build risk manager (standalone, not yet integrated into SimEngine internals)
    RiskManagerConfig rm_cfg;
    rm_cfg.max_order_notional_usd  = 100'000.0;
    rm_cfg.max_daily_notional_usd  = 5'000'000.0;
    rm_cfg.available_margin_usd    = cash;
    rm_cfg.max_position_qty["BTCUSDT"] = 0.20;
    rm_cfg.max_consecutive_losses  = 20;
    rm_cfg.max_daily_loss_usd      = cash * 0.03;
    rm_cfg.max_drawdown_pct        = 0.10;
    rm_cfg.halt_on_circuit_breaker = false;  // don't halt in test

    // Generate data
    auto events = make_data("BTCUSDT", 43500.0, 20, 55);

    // Run simulation with profiled strategy
    SimEngine engine(ps, lat, fcfg, cash, rlim, 1'000'000'000LL);
    engine.add_symbol("BTCUSDT", 0.01);

    auto stats = engine.run(events);
    CHECK(stats.strategy_errors == 0, "No strategy errors");

    // Check profiler captured timings
    const Profiler& prof = ps.profiler();
    CHECK(prof.section_count() > 0, "Profiler captured some sections");
    auto* bu = prof.get("on_book_update");
    if (bu) {
        CHECK(bu->count > 0, "on_book_update profiled");
        std::cout << "  on_book_update: " << bu->count << " calls, mean="
                  << std::setprecision(1) << bu->mean_ns() << "ns\n";
    }

    // Run analytics
    AnalyticsEngine ae(5'000'000'000LL);
    ae.ingest(stats, engine.fill_history());
    ae.ingest_pnl_series(engine.pnl_series());
    ae.ingest_latency_samples(
        engine.latency_raw_feed(),
        engine.latency_raw_order()
    );

    auto rm = ae.compute_risk_metrics();
    auto cb = ae.compute_cost_breakdown();

    CHECK(std::isfinite(rm.sharpe) || std::isnan(rm.sharpe), "Sharpe is finite or NaN");
    CHECK(rm.max_drawdown <= 0.0, "Max drawdown <= 0");
    CHECK(cb.total_notional >= 0.0, "Total notional >= 0");

    std::cout << "  Sharpe         : " << std::setprecision(3) << rm.sharpe << "\n";
    std::cout << "  Win rate       : " << std::setprecision(1) << rm.win_rate*100 << "%\n";
    std::cout << "  Net fees (bps) : " << std::setprecision(3) << cb.fee_bps << "\n";
    std::cout << "  Config keys    : " << cfg.size() << "\n";
    std::cout << "  Profiler sects : " << prof.section_count() << "\n";
}


void test_oms_reconciler() {
    SUITE("OMS Reconciler");

    OrderManager oms;

    Order o;
    o.order_id = "o1";
    o.client_id = "c1";
    o.symbol = "BTCUSDT";
    o.side = Side::Buy;
    o.price = 100.0;
    o.qty = 1.0;
    o.timestamp = 1000;

    CHECK(oms.submit(o), "OMS submit order");
    CHECK(oms.on_ack(o, true, 1100), "OMS ack accepted");

    std::vector<ExchangeOrderSnapshot> snaps;
    snaps.push_back({
        "binance",
        "o1",
        "c1",
        "BTCUSDT",
        Side::Buy,
        100.0,
        1.0,
        0.4,
        ExchangeOrderStatus::PartiallyFilled,
        1200,
        1200
    });

    OmsReconciler reconciler;
    auto report = reconciler.reconcile(oms, snaps, 40'000'000'000LL);

    CHECK(!report.clean(), "Reconciler detects mismatch");
    CHECK(report.checked_local == 1, "Checked one local order");
    CHECK(report.checked_exchange == 1, "Checked one exchange order");
    CHECK(report.matched == 1, "Matched local and exchange order");
    CHECK(report.critical > 0, "Critical mismatch detected");

    bool found_qty_fix = false;
    for (const auto& a : report.actions) {
        if (a.type == ReconcileActionType::CorrectLocalFilledQty)
            found_qty_fix = true;
    }

    CHECK(found_qty_fix, "Detected filled_qty correction action");

    auto csv = reconciler.report_to_csv(report);
    CHECK(!csv.empty(), "Reconciliation CSV non-empty");
    CHECK(csv.find("filled_qty_mismatch") != std::string::npos, "CSV includes mismatch reason");
}


void test_paper_exchange() {
    SUITE("PaperExchange");

    PaperExchange ex("paper");
    int state_changes = 0;
    int md_events = 0;
    int reports = 0;

    ex.set_state_callback([&](ExchangeConnectionState) {
        ++state_changes;
    });

    ex.set_market_data_callback([&](const MarketEvent&) {
        ++md_events;
    });

    ex.set_execution_report_callback([&](const ExchangeExecutionReport&) {
        ++reports;
    });

    CHECK(ex.connect(), "PaperExchange connect");
    CHECK(ex.state() == ExchangeConnectionState::Connected, "Exchange connected");
    CHECK(state_changes >= 1, "State callback fired");

    ExchangeSymbolInfo info;
    info.symbol = "BTCUSDT";
    info.base = "BTC";
    info.quote = "USDT";
    info.tick_size = 0.01;
    info.lot_size = 0.001;
    ex.add_symbol(info);

    auto si = ex.symbol_info("BTCUSDT");
    CHECK(si.has_value(), "symbol_info exists");
    CHECK_NEAR(si->tick_size, 0.01, 1e-12, "tick_size stored");

    CHECK(ex.subscribe_book("BTCUSDT", 20), "subscribe_book works");
    CHECK(ex.subscribe_trades("BTCUSDT"), "subscribe_trades works");

    L2Update upd;
    upd.symbol = "BTCUSDT";
    upd.side = BookSide::Bid;
    upd.price = 100.0;
    upd.qty = 1.0;
    upd.timestamp = 1000;

    ex.inject_market_event(upd);
    CHECK(md_events == 1, "Market data callback fired");

    ExchangeOrderRequest req;
    req.client_id = "c1";
    req.symbol = "BTCUSDT";
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.price = 99.0;
    req.qty = 0.01;
    req.post_only = true;

    auto ack = ex.submit_order(req);
    CHECK(ack.accepted, "submit_order accepted");
    CHECK(ex.open_orders("BTCUSDT").size() == 1, "Open order stored");
    CHECK(reports >= 1, "Execution report emitted");

    ExchangeReplaceRequest rr;
    rr.order_id = ack.order_id;
    rr.symbol = "BTCUSDT";
    rr.new_price = 98.5;
    rr.new_qty = 0.02;

    auto rack = ex.replace_order(rr);
    CHECK(rack.accepted, "replace_order accepted");

    ExchangeCancelRequest cr;
    cr.order_id = ack.order_id;
    cr.symbol = "BTCUSDT";

    auto cack = ex.cancel_order(cr);
    CHECK(cack.accepted, "cancel_order accepted");
    CHECK(ex.open_orders("BTCUSDT").empty(), "Open order removed after cancel");

    ex.disconnect();
    CHECK(ex.state() == ExchangeConnectionState::Disconnected, "Exchange disconnected");
}


void test_session_manager() {
    SUITE("SessionManager");

    SessionConfig cfg;
    cfg.venue = "binance";
    cfg.require_tls = true;
    cfg.require_auth = true;
    cfg.heartbeat_interval_ns = 1'000'000'000LL;
    cfg.stale_timeout_ns = 3'000'000'000LL;
    cfg.reconnect_base_delay_ns = 1'000'000'000LL;
    cfg.reconnect_max_delay_ns = 8'000'000'000LL;
    cfg.max_reconnects = 3;

    SessionManager sm(cfg);

    int events = 0;
    sm.set_callback([&](const SessionEvent&) {
        ++events;
    });

    sm.add_subscription({"book", "BTCUSDT", 50});
    sm.add_subscription({"trades", "BTCUSDT", 0});
    sm.add_subscription({"book", "BTCUSDT", 50}); // duplicate should not add

    CHECK(sm.subscriptions().size() == 2, "Duplicate subscription ignored");

    int64_t t = 1'000'000'000LL;

    CHECK(sm.connect(t), "Connect requested");
    CHECK(sm.state() == SessionState::Connecting, "State = Connecting");

    CHECK(sm.on_tcp_connected(t + 100), "TCP connected");
    CHECK(sm.state() == SessionState::TcpConnected, "State = TcpConnected");

    CHECK(sm.on_tls_ready(t + 200), "TLS ready");
    CHECK(sm.state() == SessionState::Authenticating, "State = Authenticating");

    CHECK(sm.on_authenticated(t + 300, t + 10'000'000'000LL), "Authenticated");
    CHECK(sm.state() == SessionState::Live, "State = Live after resubscribe");

    CHECK(sm.stats().subscriptions_sent == 2, "Subscriptions sent");

    CHECK(sm.should_send_heartbeat(t + 1'000'000'500LL), "Should send heartbeat");
    sm.on_heartbeat_sent(t + 1'000'000'500LL);
    sm.on_heartbeat_received(t + 1'000'500'500LL);

    CHECK(sm.stats().heartbeats_sent == 1, "Heartbeat sent counted");
    CHECK(sm.stats().heartbeats_received == 1, "Heartbeat received counted");
    CHECK(sm.stats().avg_heartbeat_rtt_us > 0.0, "Heartbeat RTT tracked");

    CHECK(sm.on_sequence("BTCUSDT.book", 1) == SessionEventType::Live, "Seq 1 OK");
    CHECK(sm.on_sequence("BTCUSDT.book", 2) == SessionEventType::Live, "Seq 2 OK");
    CHECK(sm.on_sequence("BTCUSDT.book", 4) == SessionEventType::SequenceGap, "Sequence gap detected");
    CHECK(sm.stats().sequence_gaps == 1, "Sequence gap counted");

    CHECK(sm.on_sequence("BTCUSDT.book", 4) == SessionEventType::DuplicateSequence, "Duplicate sequence detected");
    CHECK(sm.stats().duplicates == 1, "Duplicate counted");

    CHECK(sm.on_sequence("BTCUSDT.book", 3) == SessionEventType::OutOfOrderSequence, "Out-of-order sequence detected");
    CHECK(sm.stats().out_of_order == 1, "Out-of-order counted");

    CHECK(sm.check_stale(t + 5'000'000'000LL), "Stale detected");
    CHECK(sm.state() == SessionState::Reconnecting, "State = Reconnecting after stale");

    CHECK(!sm.should_reconnect_now(t + 5'000'000'100LL), "Reconnect not due immediately");

    sm.begin_reconnect(t + 20'000'000'000LL);
    CHECK(sm.state() == SessionState::Connecting, "Begin reconnect moves to Connecting");

    CHECK(sm.on_tcp_connected(t + 20'000'000'100LL), "Reconnect TCP connected");
    CHECK(sm.on_tls_ready(t + 20'000'000'200LL), "Reconnect TLS ready");
    CHECK(sm.on_authenticated(t + 20'000'000'300LL, t + 40'000'000'000LL), "Reconnect authenticated");

    CHECK(sm.state() == SessionState::Live, "Recovered to Live");
    CHECK(sm.stats().successful_reconnects >= 1, "Successful reconnect counted");
    CHECK(sm.stats().subscriptions_restored == 2, "Subscriptions restored");

    CHECK(sm.check_auth_expiry(t + 39'500'000'000LL), "Auth expiry detected");
    CHECK(sm.state() == SessionState::Authenticating, "State = Authenticating after auth expiry");

    auto health = sm.health();
    CHECK(health.score >= 0.0 && health.score <= 1.0, "Health score in [0,1]");
    CHECK(!sm.event_history().empty(), "Event history populated");
    CHECK(events > 0, "Callback fired");
}



void test_rest_client() {
    SUITE("RestClient");

    MockRestTransport transport;
    int calls = 0;

    transport.set_handler([&](const std::string& base, const RestRequest& req) {
        ++calls;

        RestResponse r;
        r.request_id = req.request_id;

        if (req.path == "/retry" && calls < 2) {
            r.status = 500;
            r.error = RestErrorCode::ServerError;
            r.body = R"({"error":"temporary"})";
            return r;
        }

        if (req.path == "/signed") {
            CHECK(req.headers.count("X-Signature") == 1, "Signed request has signature header");
        }

        if (req.path == "/ids") {
            CHECK(req.headers.count("X-Request-Id") == 1, "Request ID header added");
            CHECK(req.headers.count("Idempotency-Key") == 1, "Idempotency key added");
        }

        r.status = 200;
        r.error = RestErrorCode::None;
        r.body = R"({"ok":true})";
        return r;
    });

    RestClientConfig cfg;
    cfg.retry.policy = RetryPolicy::SafeOnly;
    cfg.retry.max_attempts = 3;
    cfg.retry.base_backoff_ms = 1;
    cfg.retry.max_backoff_ms = 2;
    cfg.default_rate_limit.requests_per_second = 1000;
    cfg.default_rate_limit.burst = 1000;

    RestSigner signer([](RestRequest& req) {
        req.headers["X-Signature"] = "mock-signature";
    });

    RestClient client("https://api.test", transport, cfg, signer);

    auto r1 = client.get("/ping");
    CHECK(r1.ok(), "GET /ping succeeds");
    CHECK(r1.status == 200, "GET status 200");

    calls = 0;
    auto r2 = client.get("/retry");
    CHECK(r2.ok(), "GET /retry succeeds after retry");
    CHECK(r2.attempts == 2, "Retry used 2 attempts");
    CHECK(client.stats().retries >= 1, "Retry counter incremented");

    auto r3 = client.get("/signed", {}, true);
    CHECK(r3.ok(), "Signed GET succeeds");

    auto r4 = client.post("/ids", R"({"x":1})", false, true);
    CHECK(r4.ok(), "Idempotent POST succeeds");

    CHECK(client.stats().requests >= 4, "Stats requests counted");
    CHECK(client.stats().success >= 4, "Stats success counted");
    CHECK(client.stats().avg_latency_us >= 0.0, "Latency stat valid");
}


void test_exchange_adapter() {
    SUITE("ExchangeAdapter");

    PaperExchange ex("paper");
    ex.add_symbol({"BTCUSDT", "BTC", "USDT", 0.01, 0.001, 0.001, 10.0, 100.0});

    PaperExchangeAdapter adapter(ex);
    adapter.add_symbol_mapping("BTCUSDT", "BTC-USDT");

    int md_count = 0;
    int er_count = 0;
    int err_count = 0;

    adapter.set_market_callback([&](const MarketEvent& e) {
        ++md_count;
        CHECK(std::holds_alternative<L2Update>(e), "Market event is L2Update");
        CHECK(std::get<L2Update>(e).symbol == "BTCUSDT", "Market symbol normalized to internal");
    });

    adapter.set_execution_callback([&](const ExchangeExecutionReport& r) {
        ++er_count;
        CHECK(r.symbol == "BTCUSDT", "Execution report symbol normalized");
    });

    adapter.set_error_callback([&](const AdapterErrorEvent& err) {
        ++err_count;
        CHECK(err.error != ExchangeErrorCode::None, "Adapter error callback receives error");
    });

    CHECK(adapter.connect(), "Adapter connects");

    bool book_sub = adapter.subscribe_orderbook("BTCUSDT", 20);
    bool trade_sub = adapter.subscribe_trades("BTCUSDT");

    CHECK(book_sub, "Subscribe orderbook");
    CHECK(trade_sub, "Subscribe trades");

    L2Update upd;
    upd.symbol = "BTC-USDT";
    upd.side = BookSide::Bid;
    upd.price = 100.0;
    upd.qty = 1.0;
    upd.timestamp = 1000;
    ex.inject_market_event(upd);

    CHECK(md_count == 1, "Market callback fired once");

    ExchangeOrderRequest req;
    req.client_id = "cid-1";
    req.symbol = "BTCUSDT";
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.price = 100.0;
    req.qty = 0.01;
    req.post_only = true;

    auto result = adapter.submit_order(req);
    CHECK(result.ok, "Adapter order accepted");
    CHECK(er_count >= 1, "Execution report fired for order ACK");

    auto open = adapter.open_orders("BTCUSDT");
    CHECK(!open.empty(), "Adapter open_orders non-empty");
    CHECK(open[0].symbol == "BTCUSDT", "Open order symbol normalized internal");

    auto dup = adapter.submit_order(req);
    CHECK(!dup.ok, "Duplicate client id rejected");
    CHECK(dup.error == ExchangeErrorCode::DuplicateOrder, "Duplicate mapped to DuplicateOrder");
    CHECK(err_count >= 1, "Error callback fired");

    ExchangeReplaceRequest rep;
    rep.order_id = result.ack.order_id;
    rep.client_id = req.client_id;
    rep.symbol = "BTCUSDT";
    rep.new_price = 100.01;
    rep.new_qty = 0.02;

    auto rep_result = adapter.replace_order(rep);
    CHECK(rep_result.ok, "Adapter replace accepted");

    ExchangeCancelRequest cancel;
    cancel.order_id = result.ack.order_id;
    cancel.client_id = req.client_id;
    cancel.symbol = "BTCUSDT";

    auto cancel_result = adapter.cancel_order(cancel);
    CHECK(cancel_result.ok, "Adapter cancel accepted");

    CHECK(adapter.to_external_symbol("BTCUSDT") == "BTC-USDT", "Symbol to external");
    CHECK(adapter.to_internal_symbol("BTC-USDT") == "BTCUSDT", "Symbol to internal");

    auto metrics = adapter.metrics();
    CHECK(metrics.submitted_orders >= 2, "Metrics submitted_orders updated");
    CHECK(metrics.cancelled_orders >= 1, "Metrics cancelled_orders updated");
    CHECK(metrics.replaced_orders >= 1, "Metrics replaced_orders updated");

    CHECK(adapter.translate_error("429", "rate limit") == ExchangeErrorCode::RateLimited, "Error translation rate limit");
    CHECK(adapter.translate_error("-2011", "unknown order") == ExchangeErrorCode::OrderNotFound, "Venue code translation");
}

void test_websocket_engine() {
    SUITE("WebSocketEngine");

    MockWebSocketTransport transport;

    WsConfig cfg;
    cfg.ping_interval_ns = 1;
    cfg.stale_timeout_ns = 1'000'000'000LL;
    cfg.max_outbound_queue = 4;

    WebSocketEngine ws(transport, cfg);

    int messages = 0;
    int errors = 0;
    int events = 0;

    ws.set_message_callback([&](const WsMessage& msg) {
        if (msg.type == WsMessageType::Text && msg.payload == "hello")
            ++messages;
    });

    ws.set_error_callback([&](const std::string&) {
        ++errors;
    });

    ws.set_event_callback([&](const WsEvent&) {
        ++events;
    });

    CHECK(ws.connect("wss://mock.exchange/ws"), "WebSocket connects");
    CHECK(ws.state() == WsState::Connected, "WebSocket state connected");
    CHECK(transport.url() == "wss://mock.exchange/ws", "URL stored");

    CHECK(ws.send_text("subscribe"), "send_text succeeds");
    CHECK(!transport.sent().empty(), "Transport captured outgoing message");

    transport.inject({WsMessageType::Text, "hello", 0});
    ws.poll_once();

    CHECK(messages == 1, "Text message callback fired");
    CHECK(ws.stats().messages_in >= 1, "messages_in updated");

    transport.inject({WsMessageType::Ping, "abc", 0});
    ws.poll_once();

    CHECK(ws.stats().pings_in == 1, "Ping counted");
    CHECK(ws.stats().pongs_out >= 1, "Auto pong sent");

    CHECK(ws.add_subscription("book:BTCUSDT", "{\"op\":\"sub\",\"ch\":\"book\"}"), "Subscription added");
    CHECK(ws.subscriptions().count("book:BTCUSDT") == 1, "Subscription stored");

    CHECK(ws.remove_subscription("book:BTCUSDT"), "Subscription removed");
    CHECK(ws.subscriptions().count("book:BTCUSDT") == 0, "Subscription erased");

    ws.disconnect();
    CHECK(ws.state() == WsState::Disconnected, "WebSocket disconnected");

    CHECK(ws.send_text("queued-msg"), "Send while disconnected queues message");
    CHECK(ws.reconnect(), "Reconnect succeeds");
    CHECK(ws.state() == WsState::Connected, "State connected after reconnect");

    bool found_queued = false;
    for (const auto& m : transport.sent()) {
        if (m.payload == "queued-msg") found_queued = true;
    }
    CHECK(found_queued, "Queued outbound message flushed after reconnect");

    CHECK(events > 0, "Event callback fired");
    CHECK(errors == 0, "No unexpected errors");
}


void test_binance_adapter() {
    SUITE("BinanceAdapter");

    OrderManager oms;
    PortfolioState portfolio(100000.0);

    RiskGateway risk;
    SymbolRiskLimits limits;
    limits.max_position = 10.0;
    limits.max_order_qty = 5.0;
    limits.max_order_notional = 500000.0;
    limits.tick_size = 0.01;
    limits.lot_size = 0.001;
    limits.max_orders_per_second = 100;
    risk.set_symbol_limits("BTCUSDT", limits);

    PaperExchangeGateway gateway("binance", &oms, &risk, &portfolio);

    OrderBook book("BTCUSDT", 0.01);
    book.apply_l2({"BTCUSDT", BookSide::Bid, 100.0, 5.0, 1, 1});
    book.apply_l2({"BTCUSDT", BookSide::Ask, 101.0, 5.0, 2, 2});
    gateway.update_book("BTCUSDT", &book);

    BinanceAdapterConfig cfg;
    cfg.mode = AdapterMode::Paper;
    cfg.max_orders_per_second = 100;
    BinanceAdapter adapter(cfg, &gateway, &oms);

    BinanceSymbolFilter filter;
    filter.symbol = "BTCUSDT";
    filter.tick_size = 0.01;
    filter.lot_size = 0.001;
    filter.min_qty = 0.001;
    filter.max_qty = 10.0;
    filter.min_notional = 1.0;
    adapter.set_symbol_filter(filter);

    int book_cb = 0;
    int trade_cb = 0;
    int ack_cb = 0;
    int fill_cb = 0;
    int cancel_cb = 0;
    int err_cb = 0;

    AdapterCallbacks cb;
    cb.on_book = [&](const L2Update& u) {
        ++book_cb;
        CHECK(u.symbol == "BTCUSDT", "Book callback symbol normalized");
    };
    cb.on_trade = [&](const Trade& t) {
        ++trade_cb;
        CHECK(t.symbol == "BTCUSDT", "Trade callback symbol normalized");
    };
    cb.on_ack = [&](const Order&, bool accepted) {
        ++ack_cb;
        CHECK(accepted || ack_cb > 1, "ACK callback fired");
    };
    cb.on_fill = [&](const FillEvent& f) {
        ++fill_cb;
        CHECK(f.symbol == "BTCUSDT", "Fill callback symbol normalized");
    };
    cb.on_cancel = [&](const std::string&) {
        ++cancel_cb;
    };
    cb.on_error = [&](const AdapterErrorEvent& err) {
        ++err_cb;
        CHECK(err.error != ExchangeErrorCode::None, "Binance adapter error callback receives error");
    };
    adapter.set_callbacks(cb);

    CHECK(adapter.connect(), "Adapter connects");
    CHECK(adapter.is_connected(), "Adapter state connected");
    CHECK(adapter.authenticated(), "Adapter authenticated in paper mode");

    CHECK(adapter.subscribe_orderbook("btc-usdt", 20), "Subscribe orderbook");
    CHECK(adapter.subscribe_trades("BTC/USDT"), "Subscribe trades");
    CHECK(adapter.subscribe_ticker("btcusdt"), "Subscribe ticker");

    adapter.inject_book_update({"btc-usdt", BookSide::Bid, 100.0, 1.0, 10, 10});
    CHECK(book_cb == 1, "Book callback fired once");

    adapter.inject_trade({"t1", "btc/usdt", Side::Buy, 101.0, 0.1, 11, Side::Buy, false});
    CHECK(trade_cb == 1, "Trade callback fired once");

    Order o;
    o.order_id = "o1";
    o.client_id = "c1";
    o.symbol = "btc-usdt";
    o.side = Side::Buy;
    o.price = 100.0;
    o.qty = 0.01;
    o.timestamp = 12;
    o.order_type = OrderType::PostOnly;

    auto ack = adapter.submit_order(o);
    CHECK(ack.accepted, "Adapter order accepted");
    CHECK(ack.order_id == "o1", "ACK order id preserved");
    CHECK(ack_cb >= 1, "ACK callback fired");
    CHECK(!adapter.open_orders("BTCUSDT").empty(), "Adapter open_orders non-empty");

    Order dup = o;
    dup.order_id = "o2";
    dup.client_id = "c1";
    auto dup_ack = adapter.submit_order(dup);
    CHECK(!dup_ack.accepted, "Duplicate client id rejected");
    CHECK(err_cb >= 1, "Error callback fired for duplicate");

    FillEvent f;
    f.order_id = "o1";
    f.symbol = "btc-usdt";
    f.side = Side::Buy;
    f.price = 100.0;
    f.qty = 0.01;
    f.timestamp = 13;
    f.is_maker = true;

    adapter.inject_fill(f);
    CHECK(fill_cb == 1, "Fill callback fired once");
    CHECK(adapter.fills().size() == 1, "Adapter stores fill");

    auto rep = adapter.replace_order("o1", 99.99, 0.01);
    CHECK(rep.accepted || rep.reject_code == GatewayRejectCode::UnknownOrder, "Replace returns valid result");

    auto cack = adapter.cancel_order("o1");
    CHECK(cack.accepted || cack.reject_code == GatewayRejectCode::UnknownOrder, "Cancel returns valid result");

    CHECK(BinanceAdapter::normalize_to_internal("btc-usdt") == "BTCUSDT", "Symbol to internal");
    CHECK(BinanceAdapter::symbol_to_external("btc/usdt") == "BTCUSDT", "Symbol to external");

    CHECK(BinanceAdapter::translate_binance_error(-1003) == ExchangeErrorCode::RateLimited,
          "Error translation rate limit");
    CHECK(BinanceAdapter::translate_binance_error(-2011) == ExchangeErrorCode::OrderNotFound,
          "Error translation order not found");

    const auto& st = adapter.stats();
    CHECK(st.submitted_orders >= 2, "Metrics submitted_orders updated");
    CHECK(st.accepted_orders >= 1, "Metrics accepted_orders updated");
    CHECK(st.errors >= 1, "Metrics errors updated");

    adapter.heartbeat();
    adapter.disconnect();
    CHECK(!adapter.is_connected(), "Adapter disconnected");
}


// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
        std::cout << "==================================================\n";
        std::cout << "  HFT Production Modules — Test Suite            \n";
        std::cout << "==================================================\n";


    test_inventory_manager();
    test_risk_manager();
    test_profiler();
    test_portfolio_risk_engine();
    test_config();
    test_rest_client();
    test_stat_arb();
    test_execution_algos();
    test_paper_exchange();
    test_exchange_adapter();
    test_websocket_engine();
    test_binance_adapter();
    test_session_manager();
    test_full_integration();
    test_oms_reconciler();
    

        std::cout << "==================================================\n";
    std::cout << "  Results: " << g_pass << " passed, " << g_fail << " failed\n";
    if (g_fail == 0) std::cout << "  ✓ ALL TESTS PASSED\n";
    else             std::cout << "  ✗ " << g_fail << " FAILURES\n";
        std::cout << "==================================================\n";
    return g_fail > 0 ? 1 : 0;
}