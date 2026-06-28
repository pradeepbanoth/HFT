// ─────────────────────────────────────────────────────────────────────────────
// examples/bybit_multiasset_mm.cpp  —  Bybit BTC+ETH correlated MM example
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/hft.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>

using namespace hft;

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║   Bybit BTC/USDT + ETH/USDT  —  Multi-Asset GLFT MM     ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";

    // ── Correlated Heston data for BTC + ETH ─────────────────────────────────
    AssetParams btc_p;
    btc_p.symbol         = "BTCUSDT";
    btc_p.initial_price  = 43'500.0;
    btc_p.annual_vol     = 0.75;
    btc_p.vol_of_vol     = 0.40;
    btc_p.vol_mean       = 0.75;
    btc_p.vol_vol        = 0.50;
    btc_p.tick_size      = 0.50;        // Bybit BTC tick
    btc_p.spread_mean    = 1.00;
    btc_p.depth_levels   = 15;
    btc_p.trade_intensity= 8.0;
    btc_p.hawkes_alpha   = 0.30;
    btc_p.seed           = 100;

    AssetParams eth_p;
    eth_p.symbol         = "ETHUSDT";
    eth_p.initial_price  = 2'250.0;
    eth_p.annual_vol     = 0.85;
    eth_p.vol_of_vol     = 0.45;
    eth_p.vol_mean       = 0.80;
    eth_p.vol_vol        = 0.55;
    eth_p.tick_size      = 0.05;        // Bybit ETH tick
    eth_p.spread_mean    = 0.10;
    eth_p.depth_levels   = 15;
    eth_p.trade_intensity= 10.0;
    eth_p.hawkes_alpha   = 0.35;
    eth_p.seed           = 101;

    // BTC–ETH price correlation matrix (2×2, row-major)
    std::vector<double> price_corr = {1.00, 0.85, 0.85, 1.00};
    std::vector<double> vol_corr   = {1.00, 0.70, 0.70, 1.00};

    std::cout << "Generating 2h correlated BTC+ETH tick data (Heston + Hawkes)...\n";
    auto t0 = std::chrono::high_resolution_clock::now();
    CorrelatedGenerator cgen({btc_p, eth_p}, price_corr, vol_corr);
    auto events = cgen.generate(60.0, 1000);   // 2 hours at 100 µs
    auto t1 = std::chrono::high_resolution_clock::now();

    int n_btc_l2=0, n_eth_l2=0, n_btc_tr=0, n_eth_tr=0;
    for (auto& e : events) std::visit([&](const auto& ev){
        using T = std::decay_t<decltype(ev)>;
        if constexpr (std::is_same_v<T,L2Update>) {
            if (ev.symbol=="BTCUSDT") ++n_btc_l2; else ++n_eth_l2;
        }
        if constexpr (std::is_same_v<T,Trade>) {
            if (ev.symbol=="BTCUSDT") ++n_btc_tr; else ++n_eth_tr;
        }
    }, e);

    std::cout << "  Generated " << events.size() << " events in "
              << std::fixed << std::setprecision(2)
              << std::chrono::duration<double>(t1-t0).count() << "s\n"
              << "  BTC: " << n_btc_l2 << " L2, " << n_btc_tr << " trades\n"
              << "  ETH: " << n_eth_l2 << " L2, " << n_eth_tr << " trades\n\n";

    // ── Asset configs ─────────────────────────────────────────────────────────
    AssetConfig btc_cfg;
    btc_cfg.symbol         = "BTCUSDT";
    btc_cfg.tick_size      = 0.50;
    btc_cfg.lot_size       = 0.001;
    btc_cfg.quote_qty      = 0.01;
    btc_cfg.max_inventory  = 0.15;
    btc_cfg.as_gamma       = 0.10;
    btc_cfg.as_k           = 1.8;
    btc_cfg.as_T           = 300.0;
    btc_cfg.min_spread_bps = 1.0;
    btc_cfg.max_spread_bps = 25.0;
    btc_cfg.skew_factor    = 0.0004;
    btc_cfg.use_glft       = true;
    btc_cfg.toxicity_pause = 0.80;
    btc_cfg.max_quote_age_ms = 4000.0;
    btc_cfg.min_refresh_us = 300.0;

    AssetConfig eth_cfg;
    eth_cfg.symbol         = "ETHUSDT";
    eth_cfg.tick_size      = 0.05;
    eth_cfg.lot_size       = 0.01;
    eth_cfg.quote_qty      = 0.10;
    eth_cfg.max_inventory  = 2.0;
    eth_cfg.as_gamma       = 0.12;
    eth_cfg.as_k           = 1.6;
    eth_cfg.as_T           = 300.0;
    eth_cfg.min_spread_bps = 1.2;
    eth_cfg.max_spread_bps = 28.0;
    eth_cfg.skew_factor    = 0.0004;
    eth_cfg.use_glft       = true;
    eth_cfg.toxicity_pause = 0.80;
    eth_cfg.max_quote_age_ms = 4000.0;
    eth_cfg.min_refresh_us = 300.0;

    // ── Correlation matrix for cross-asset skew ───────────────────────────────
    std::vector<std::vector<double>> corr_matrix = {{1.00, 0.85},{0.85, 1.00}};

    // ── Strategy ──────────────────────────────────────────────────────────────
    MultiAssetMarketMaker mm(
        {btc_cfg, eth_cfg},
        /*quote_refresh_tol=*/0.0002,
        /*enable_cross_hedge=*/true,
        corr_matrix
    );

    // ── Risk limits ───────────────────────────────────────────────────────────
    RiskLimits limits;
    limits.max_position["BTCUSDT"] = 0.30;
    limits.max_position["ETHUSDT"] = 4.0;
    limits.max_drawdown            = 0.10;
    limits.max_daily_loss          = 0.03;
    limits.halt_on_breach          = true;

    // ── Fill model (Bybit fees) ────────────────────────────────────────────────
    FillModelConfig fill_cfg;
    fill_cfg.maker_fee       = -0.0001;   // Bybit maker rebate
    fill_cfg.taker_fee       =  0.0006;
    fill_cfg.fill_mode       = FillMode::FIFO;
    fill_cfg.ac_gamma        = 1e-6;
    fill_cfg.ac_eta          = 1e-7;
    fill_cfg.adverse_penalty = 0.5;
    fill_cfg.adverse_thresh  = 0.70;

    // ── Engine ────────────────────────────────────────────────────────────────
    SimEngine engine(mm, bybit_colocation(), fill_cfg,
                     100'000.0, limits,
                     /*snapshot_interval_ns=*/1'000'000'000LL);
    engine.add_symbol("BTCUSDT", 0.50);
    engine.add_symbol("ETHUSDT", 0.05);

    // ── Run ───────────────────────────────────────────────────────────────────
    std::cout << "Running simulation...\n";
    auto stats = engine.run(events);

    // ── Results ───────────────────────────────────────────────────────────────
    const auto& ps = stats.portfolio_summary;
    double maker_pct = stats.total_fills > 0
        ? 100.0 * stats.maker_fills / stats.total_fills : 0.0;

    std::cout << "\n";
    std::cout << "┌─────────────────────────────────────────────────────────┐\n";
    std::cout << "│  Performance Summary  —  Bybit Multi-Asset MM           │\n";
    std::cout << "├─────────────────────────────────────────────────────────┤\n";
    std::cout << std::fixed;
    std::cout << "│  Ticks processed : " << std::setw(16) << stats.ticks_processed << "               │\n";
    std::cout << "│  Throughput      : " << std::setw(13) << std::setprecision(0)
              << stats.ticks_per_second << " ticks/sec          │\n";
    std::cout << "├─────────────────────────────────────────────────────────┤\n";
    std::cout << "│  P&L             : $" << std::setw(12) << std::setprecision(4) << ps.pnl
              << " (" << std::setw(6) << std::setprecision(3) << ps.pnl_pct << "%)    │\n";
    std::cout << "│  Total fees      : $" << std::setw(14) << std::setprecision(4)
              << ps.total_fees << "               │\n";

    for (auto& [sym, rpnl] : ps.realized_pnl) {
        std::cout << "│  Realized " << std::setw(8) << sym
                  << " : $" << std::setw(12) << std::setprecision(4) << rpnl
                  << "               │\n";
    }
    std::cout << "├─────────────────────────────────────────────────────────┤\n";
    std::cout << "│  Total fills     : " << std::setw(16) << stats.total_fills << "               │\n";
    std::cout << "│  Maker ratio     : " << std::setw(15) << std::setprecision(1)
              << maker_pct << "%               │\n";
    std::cout << "│  Max drawdown    : " << std::setw(14) << std::setprecision(3)
              << ps.max_drawdown*100.0 << "%               │\n";
    std::cout << "│  Sharpe ratio    : " << std::setw(16) << std::setprecision(3)
              << ps.sharpe << "               │\n";
    std::cout << "│  Calmar ratio    : " << std::setw(16) << std::setprecision(3)
              << ps.calmar << "               │\n";
    std::cout << "├─────────────────────────────────────────────────────────┤\n";
    std::cout << "│  Quotes sent     : " << std::setw(16) << mm.quote_count << "               │\n";
    std::cout << "│  Cancels         : " << std::setw(16) << mm.cancel_count << "               │\n";
    std::cout << "│  Stale cancels   : " << std::setw(16) << mm.stale_cancel_count << "               │\n";
    std::cout << "│  Tox. pauses     : " << std::setw(16) << mm.pause_count << "               │\n";
    std::cout << "├─────────────────────────────────────────────────────────┤\n";
    std::cout << "│  Feed p50/p99    : " << std::setprecision(1)
              << stats.feed_latency.p50 << " / " << stats.feed_latency.p99 << " µs              │\n";
    std::cout << "│  Order p50/p99   : "
              << stats.order_latency.p50 << " / " << stats.order_latency.p99 << " µs              │\n";
    std::cout << "│  Strategy errors : " << std::setw(16) << stats.strategy_errors << "               │\n";
    std::cout << "│  Halted          : " << std::setw(16) << (stats.halted?"YES":"no") << "               │\n";
    std::cout << "└─────────────────────────────────────────────────────────┘\n";

    std::cout << "\n  Positions at close:\n";
    for (auto& [sym, qty] : ps.positions) {
        std::cout << "    " << std::setw(10) << sym << " : "
                  << std::setprecision(6) << qty << "\n";
    }
    std::cout << "    " << std::setw(10) << "USDT" << " : $"
              << std::setprecision(2) << ps.cash << "\n\n";

    return stats.strategy_errors > 0 ? 1 : 0;
}