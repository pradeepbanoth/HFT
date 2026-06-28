// ─────────────────────────────────────────────────────────────────────────────
// examples/binance_btc_mm.cpp  —  Binance BTC/USDT market-making example
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/hft.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>

using namespace hft;

int main() {
    std::cout << "╔══════════════════════════════════════════════════════╗\n";
    std::cout << "║   Binance BTC/USDT  —  GLFT Market-Making Backtest  ║\n";
    std::cout << "╚══════════════════════════════════════════════════════╝\n\n";

    // ── Synthetic data: 1 hour of Heston BTC/USDT tick data ──────────────────
    AssetParams params;
    params.symbol          = "BTCUSDT";
    params.initial_price   = 43'500.0;
    params.annual_vol      = 0.75;
    params.vol_of_vol      = 0.40;
    params.vol_mean        = 0.75;
    params.vol_vol         = 0.50;
    params.price_vol_corr  = -0.65;
    params.tick_size       = 0.01;
    params.spread_mean     = 0.50;
    params.spread_kappa    = 5.0;
    params.spread_vol      = 0.02;
    params.depth_levels    = 15;
    params.depth_base_qty  = 1.0;
    params.depth_decay     = 0.6;
    params.trade_intensity = 8.0;
    params.hawkes_alpha    = 0.30;
    params.hawkes_beta     = 2.00;
    params.regime_switch_rate = 0.002;
    params.seed            = 42;

    std::cout << "Generating 1h of BTC/USDT tick data (Heston + Hawkes)...\n";
    auto t_gen0 = std::chrono::high_resolution_clock::now();
    SyntheticGenerator gen(params);
    auto events = gen.generate(60.0, 1000);   // 1 hour at 100 µs intervals
    auto t_gen1 = std::chrono::high_resolution_clock::now();
    double gen_s = std::chrono::duration<double>(t_gen1 - t_gen0).count();

    int n_l2 = 0, n_trades = 0;
    for (auto& e : events)
        std::visit([&](const auto& ev){
            using T = std::decay_t<decltype(ev)>;
            if constexpr (std::is_same_v<T,L2Update>) ++n_l2;
            if constexpr (std::is_same_v<T,Trade>)    ++n_trades;
        }, e);

    std::cout << "  Generated " << events.size() << " events in "
              << std::fixed << std::setprecision(2) << gen_s << "s\n"
              << "  L2 updates: " << n_l2 << "  Trades: " << n_trades << "\n\n";

    // ── Asset config ──────────────────────────────────────────────────────────
    AssetConfig cfg;
    cfg.symbol          = "BTCUSDT";
    cfg.tick_size       = 0.01;
    cfg.lot_size        = 0.001;
    cfg.quote_qty       = 0.01;
    cfg.max_inventory   = 0.10;
    cfg.vol_halflife    = 300;
    cfg.as_gamma        = 0.15;
    cfg.as_k            = 1.5;
    cfg.as_T            = 600.0;
    cfg.min_spread_bps  = 1.5;
    cfg.max_spread_bps  = 30.0;
    cfg.skew_factor     = 0.0003;
    cfg.toxicity_pause  = 0.80;
    cfg.max_quote_age_ms= 5000.0;
    cfg.min_refresh_us  = 200.0;
    cfg.depth_fraction  = 0.10;
    cfg.kalman_Q        = 1e-5;
    cfg.kalman_R        = 1.0;
    cfg.use_glft        = true;
    cfg.regime_halflife = 500;

    // ── Risk limits ───────────────────────────────────────────────────────────
    RiskLimits limits;
    limits.max_position["BTCUSDT"] = 0.20;
    limits.max_drawdown            = 0.10;
    limits.max_daily_loss          = 0.03;
    limits.halt_on_breach          = true;

    // ── Fill model ────────────────────────────────────────────────────────────
    FillModelConfig fill_cfg;
    fill_cfg.maker_fee       = -0.0002;   // Binance VIP0 maker rebate
    fill_cfg.taker_fee       =  0.0004;
    fill_cfg.fill_mode       = FillMode::FIFO;
    fill_cfg.ac_gamma        = 1e-6;
    fill_cfg.ac_eta          = 1e-7;
    fill_cfg.adverse_penalty = 0.5;
    fill_cfg.adverse_thresh  = 0.70;

    // ── Engine ────────────────────────────────────────────────────────────────
    MultiAssetMarketMaker mm({cfg}, 0.00015);
    SimEngine engine(mm, binance_colocation(), fill_cfg,
                     50'000.0, limits,
                     /*snapshot_interval_ns=*/1'000'000'000LL);
    engine.add_symbol("BTCUSDT", 0.01);

    // ── Run ───────────────────────────────────────────────────────────────────
    std::cout << "Running simulation...\n";
    auto stats = engine.run(events);

    // ── Results ───────────────────────────────────────────────────────────────
    const auto& ps = stats.portfolio_summary;
    double maker_pct = stats.total_fills > 0
        ? 100.0 * stats.maker_fills / stats.total_fills : 0.0;

    std::cout << "\n";
    std::cout << "┌─────────────────────────────────────────────────────┐\n";
    std::cout << "│  Performance Summary  —  Binance BTC/USDT           │\n";
    std::cout << "├─────────────────────────────────────────────────────┤\n";
    std::cout << "│  Ticks processed : " << std::setw(14) << stats.ticks_processed << "                 │\n";
    std::cout << "│  Wall time       : " << std::setw(13) << std::setprecision(2) << stats.wall_time_s << "s                │\n";
    std::cout << "│  Throughput      : " << std::setw(10) << std::setprecision(0) << stats.ticks_per_second << " ticks/sec          │\n";
    std::cout << "├─────────────────────────────────────────────────────┤\n";
    std::cout << "│  P&L             : $" << std::setw(10) << std::setprecision(4) << ps.pnl
              << " (" << std::setw(7) << std::setprecision(3) << ps.pnl_pct << "%)    │\n";
    std::cout << "│  Total fees      : $" << std::setw(12) << std::setprecision(4) << ps.total_fees << "              │\n";
    std::cout << "│  Realized PnL    : $" << std::setw(12)
              << (ps.realized_pnl.count("BTCUSDT") ? ps.realized_pnl.at("BTCUSDT") : 0.0) << "              │\n";
    std::cout << "├─────────────────────────────────────────────────────┤\n";
    std::cout << "│  Total fills     : " << std::setw(14) << stats.total_fills << "                 │\n";
    std::cout << "│  Maker fills     : " << std::setw(14) << stats.maker_fills << "                 │\n";
    std::cout << "│  Maker ratio     : " << std::setw(13) << std::setprecision(1) << maker_pct << "%                │\n";
    std::cout << "├─────────────────────────────────────────────────────┤\n";
    std::cout << "│  Max drawdown    : " << std::setw(13) << std::setprecision(3) << ps.max_drawdown*100 << "%                │\n";
    std::cout << "│  Sharpe ratio    : " << std::setw(14) << std::setprecision(3) << ps.sharpe << "                 │\n";
    std::cout << "│  Calmar ratio    : " << std::setw(14) << std::setprecision(3) << ps.calmar << "                 │\n";
    std::cout << "├─────────────────────────────────────────────────────┤\n";
    std::cout << "│  Quotes sent     : " << std::setw(14) << mm.quote_count << "                 │\n";
    std::cout << "│  Cancels sent    : " << std::setw(14) << mm.cancel_count << "                 │\n";
    std::cout << "│  Stale cancels   : " << std::setw(14) << mm.stale_cancel_count << "                 │\n";
    std::cout << "│  Tox. pauses     : " << std::setw(14) << mm.pause_count << "                 │\n";
    std::cout << "├─────────────────────────────────────────────────────┤\n";
    std::cout << "│  Feed p50/p99    : "
              << std::setprecision(1) << stats.feed_latency.p50 << " / "
              << stats.feed_latency.p99 << " µs              │\n";
    std::cout << "│  Order p50/p99   : "
              << stats.order_latency.p50 << " / "
              << stats.order_latency.p99 << " µs              │\n";
    std::cout << "│  Strategy errors : " << std::setw(14) << stats.strategy_errors << "                 │\n";
    std::cout << "│  Halted          : " << std::setw(14) << (stats.halted?"YES":"no") << "                 │\n";
    std::cout << "└─────────────────────────────────────────────────────┘\n";

    double btc_pos = ps.positions.count("BTCUSDT")
    ? ps.positions.at("BTCUSDT")
    : 0.0;

std::cout << "\n  BTC position : "
          << std::setprecision(6) << btc_pos;
    std::cout << " BTC\n";
    std::cout << "  USDT cash   : $" << std::setprecision(2) << ps.cash << "\n\n";

    return stats.strategy_errors > 0 ? 1 : 0;
}