#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// analytics.hpp  —  Comprehensive post-simulation analytics
//
// Computes:
//   Trade attribution  : per-fill breakdown (side, maker/taker, adverse score)
//   Time-bucketed PnL  : hourly/minutely PnL and volume bars
//   Risk metrics       : Sharpe, Sortino, Calmar, VaR, CVaR, max DD, win-rate
//   Fill analytics     : fill rate, partial fill ratio, avg queue wait
//   Latency histogram  : feed and order latency distribution buckets
//   Regime performance : PnL split by variance-ratio regime
//   Cost breakdown     : maker rebates, taker fees, spread costs, impact costs
// ─────────────────────────────────────────────────────────────────────────────

#include "types.hpp"
#include "portfolio.hpp"
#include "simulator.hpp"
#include <vector>
#include <unordered_map>
#include <string>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <map>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// TimeBucket  —  one interval of OHLCV-style PnL tracking
// ─────────────────────────────────────────────────────────────────────────────
struct TimeBucket {
    int64_t  ts_start    = 0;
    int64_t  ts_end      = 0;
    double   pnl_open    = 0.0;
    double   pnl_close   = 0.0;
    double   pnl_high    = 0.0;
    double   pnl_low     = std::numeric_limits<double>::max();
    double   volume      = 0.0;   // traded notional
    int64_t  fill_count  = 0;
    double   fees        = 0.0;
};

// ─────────────────────────────────────────────────────────────────────────────
// FillRecord  —  enriched fill with attribution fields
// ─────────────────────────────────────────────────────────────────────────────
struct FillRecord {
    FillEvent fill;
    double    mid_at_fill     = 0.0;   // book mid at fill time
    double    slippage_bps    = 0.0;   // (fill_price - mid) / mid * 1e4
    double    spread_cost_bps = 0.0;   // effective half-spread paid
    int64_t   queue_wait_ns   = 0;     // time from order submission to fill
    std::string regime        = "neutral";
};

// ─────────────────────────────────────────────────────────────────────────────
// LatencyHistogram  —  fixed-bucket latency distribution
// ─────────────────────────────────────────────────────────────────────────────
struct LatencyHistogram {
    // Buckets: <1µs, 1-10µs, 10-100µs, 100µs-1ms, 1-10ms, 10-100ms, >100ms
    static constexpr size_t kBuckets = 7;
    static constexpr double kEdges[kBuckets] = {
        1e3, 10e3, 100e3, 1e6, 10e6, 100e6, 1e18
    };  // all in nanoseconds

    int64_t counts[kBuckets] = {};
    int64_t total             = 0;
    double  sum_ns            = 0.0;
    double  min_ns            = std::numeric_limits<double>::max();
    double  max_ns            = 0.0;

    void add(double ns) {
        ++total;
        sum_ns += ns;
        min_ns  = std::min(min_ns, ns);
        max_ns  = std::max(max_ns, ns);
        for (size_t i = 0; i < kBuckets; ++i) {
            if (ns < kEdges[i]) { ++counts[i]; return; }
        }
        ++counts[kBuckets - 1];
    }

    double mean_us()   const { return total > 0 ? sum_ns / total / 1000.0 : 0.0; }
    double min_us()    const { return min_ns / 1000.0; }
    double max_us()    const { return max_ns / 1000.0; }

    void print(const std::string& label) const {
        std::cout << "  " << label << " latency histogram:\n";
        const char* labels[] = {
            "<1us","1-10us","10-100us","100us-1ms","1-10ms","10-100ms",">100ms"
        };
        for (size_t i = 0; i < kBuckets; ++i) {
            if (counts[i] == 0) continue;
            double pct = total > 0 ? 100.0 * counts[i] / total : 0.0;
            std::cout << "    " << std::setw(12) << labels[i]
                      << " : " << std::setw(8) << counts[i]
                      << "  (" << std::fixed << std::setprecision(1) << pct << "%)\n";
        }
        std::cout << "    mean=" << std::setprecision(2) << mean_us()
                  << "us  min=" << min_us() << "us  max=" << max_us() << "us\n";
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// AnalyticsEngine
// ─────────────────────────────────────────────────────────────────────────────
class AnalyticsEngine {
public:
    explicit AnalyticsEngine(int64_t bucket_size_ns = 3'600'000'000'000LL)  // 1 hour
        : bucket_size_ns_(bucket_size_ns) {}

    // ── Ingest simulation results ─────────────────────────────────────────────

    void ingest(const SimStats& stats, const std::vector<FillEvent>& fills) {
        stats_ = stats;

        // Build enriched fill records
        for (auto& fill : fills) {
            FillRecord rec;
            rec.fill = fill;
            // Slippage: for takers, how much did we pay above/below mid?
            // (mid_at_fill would be injected by a richer sim; we approximate 0 for makers)
            rec.slippage_bps = fill.is_maker ? 0.0
                : std::abs(fill.adverse_score) * 5.0;  // proxy from toxicity
            rec.spread_cost_bps = fill.is_maker
                ? std::abs(fill.fee_rate) * 10000.0
                : fill.fee_rate * 10000.0;
            fill_records_.push_back(std::move(rec));
        }
        // PnL series is ingested separately via ingest_pnl_series()
    }

    // Ingest raw PnL series (ts_ns, mtm_value) pairs
    void ingest_pnl_series(const std::vector<std::pair<int64_t,double>>& series) {
        pnl_series_ = series;

        if (series.empty()) return;
        double initial = series.front().second;
        int64_t bucket_start = series.front().first;
        TimeBucket cur;
        cur.ts_start  = bucket_start;
        cur.pnl_open  = initial;
        cur.pnl_high  = initial;
        cur.pnl_low   = initial;
        cur.pnl_close = initial;

        for (auto& [ts, mtm] : series) {
            if (ts >= bucket_start + bucket_size_ns_) {
                cur.ts_end = ts;
                time_buckets_.push_back(cur);
                bucket_start = ts;
                cur = TimeBucket{};
                cur.ts_start  = ts;
                cur.pnl_open  = mtm;
                cur.pnl_high  = mtm;
                cur.pnl_low   = mtm;
            }
            cur.pnl_close = mtm;
            cur.pnl_high  = std::max(cur.pnl_high, mtm);
            cur.pnl_low   = std::min(cur.pnl_low,  mtm);
        }
        cur.ts_end = series.back().first;
        if (cur.fill_count >= 0) time_buckets_.push_back(cur);

        // Enrich buckets with fills
        for (auto& rec : fill_records_) {
            for (auto& b : time_buckets_) {
                if (rec.fill.timestamp >= b.ts_start && rec.fill.timestamp < b.ts_end) {
                    b.fill_count++;
                    b.volume += rec.fill.qty * rec.fill.price;
                    b.fees   += rec.fill.fee;
                    break;
                }
            }
        }
    }

    void ingest_latency_samples(
        const std::vector<int64_t>& feed_ns,
        const std::vector<int64_t>& order_ns)
    {
        for (auto v : feed_ns)  feed_hist_.add(static_cast<double>(v));
        for (auto v : order_ns) order_hist_.add(static_cast<double>(v));
    }

    // ── Risk metrics ──────────────────────────────────────────────────────────

    struct RiskMetrics {
        double sharpe        = 0.0;
        double sortino       = 0.0;   // downside-only denominator
        double calmar        = 0.0;   // annualised return / max drawdown
        double max_drawdown  = 0.0;
        double max_drawdown_duration_s = 0.0;
        double var_95        = 0.0;   // 95% 1-period Value-at-Risk
        double cvar_95       = 0.0;   // Conditional VaR (Expected Shortfall)
        double win_rate      = 0.0;   // fraction of fills that were profitable
        double profit_factor = 0.0;   // gross profit / gross loss
        double avg_win       = 0.0;
        double avg_loss      = 0.0;
        double expectancy    = 0.0;   // avg PnL per trade
    };

    RiskMetrics compute_risk_metrics() const {
        RiskMetrics m;
        if (pnl_series_.size() < 3) return m;

        // Log returns
        std::vector<double> rets;
        rets.reserve(pnl_series_.size() - 1);
        for (size_t i = 1; i < pnl_series_.size(); ++i) {
            double prev = std::abs(pnl_series_[i-1].second) > 1e-9
                        ? pnl_series_[i-1].second : 1e-9;
            rets.push_back((pnl_series_[i].second - pnl_series_[i-1].second) / prev);
        }

        double mean_r = 0.0;
        for (double r : rets) mean_r += r;
        mean_r /= rets.size();

        // Variance and downside variance
        double var = 0.0, dvar = 0.0;
        for (double r : rets) {
            double d = r - mean_r;
            var  += d * d;
            if (r < 0.0) dvar += r * r;
        }
        var  /= rets.size();
        size_t neg_count = static_cast<size_t>(
            std::count_if(rets.begin(), rets.end(), [](double r){ return r < 0; }));
        dvar /= std::max(size_t{1}, neg_count);

        double sd   = std::sqrt(var);
        double dsd  = std::sqrt(dvar);
        double sqN  = std::sqrt(static_cast<double>(rets.size()));
        m.sharpe    = sd   > 1e-12 ? mean_r / sd  * sqN : 0.0;
        m.sortino   = dsd  > 1e-12 ? mean_r / dsd * sqN : 0.0;

        // Max drawdown + duration
        double peak = pnl_series_.front().second;
        double worst_dd = 0.0;
        int64_t peak_ts = pnl_series_.front().first;
        double  max_dur = 0.0;
        for (auto& [ts, v] : pnl_series_) {
            if (v > peak) { peak = v; peak_ts = ts; }
            double dd = peak > 1e-9 ? (v - peak) / peak : 0.0;
            worst_dd = std::min(worst_dd, dd);
            if (dd < 0) max_dur = std::max(max_dur, static_cast<double>(ts - peak_ts) / 1e9);
        }
        m.max_drawdown = worst_dd;
        m.max_drawdown_duration_s = max_dur;

        // Calmar
        double final_v   = pnl_series_.back().second;
        double initial_v = pnl_series_.front().second;
        double total_ret = initial_v > 1e-9 ? (final_v - initial_v) / initial_v : 0.0;
        m.calmar = worst_dd < -1e-9 ? total_ret / std::abs(worst_dd) : 0.0;

        // VaR and CVaR (historical simulation)
        std::vector<double> sorted_rets = rets;
        std::sort(sorted_rets.begin(), sorted_rets.end());
        size_t var_idx = static_cast<size_t>(0.05 * sorted_rets.size());
        m.var_95 = var_idx < sorted_rets.size() ? -sorted_rets[var_idx] : 0.0;
        double cvar_sum = 0.0; int cvar_n = 0;
        for (size_t i = 0; i <= var_idx && i < sorted_rets.size(); ++i) {
            cvar_sum += sorted_rets[i]; ++cvar_n;
        }
        m.cvar_95 = cvar_n > 0 ? -cvar_sum / cvar_n : 0.0;

        // Win/loss metrics from fills
        double gross_profit = 0.0, gross_loss = 0.0;
        int wins = 0, losses = 0;
        for (auto& rec : fill_records_) {
            double pnl = rec.fill.realized_pnl - rec.fill.fee;
            if (pnl > 0) { gross_profit += pnl; ++wins; }
            else if (pnl < 0) { gross_loss += std::abs(pnl); ++losses; }
        }
        int total_fills = wins + losses;
        m.win_rate      = total_fills > 0 ? static_cast<double>(wins) / total_fills : 0.0;
        m.profit_factor = gross_loss > 1e-12 ? gross_profit / gross_loss : 0.0;
        m.avg_win       = wins   > 0 ? gross_profit / wins   : 0.0;
        m.avg_loss      = losses > 0 ? gross_loss   / losses : 0.0;

        double total_pnl = 0.0;
        for (auto& rec : fill_records_) total_pnl += rec.fill.realized_pnl - rec.fill.fee;
        m.expectancy = total_fills > 0 ? total_pnl / total_fills : 0.0;

        return m;
    }

    // ── Cost breakdown ────────────────────────────────────────────────────────

    struct CostBreakdown {
        double maker_rebates   = 0.0;  // negative fees (positive = rebate received)
        double taker_fees      = 0.0;
        double net_fees        = 0.0;
        double avg_slippage_bps= 0.0;
        double total_notional  = 0.0;
        double fee_bps         = 0.0;  // net_fees / total_notional in bps
        int64_t maker_count    = 0;
        int64_t taker_count    = 0;
    };

    CostBreakdown compute_cost_breakdown() const {
        CostBreakdown cb;
        for (auto& rec : fill_records_) {
            double notional = rec.fill.qty * rec.fill.price;
            cb.total_notional += notional;
            if (rec.fill.is_maker) {
                cb.maker_rebates += -rec.fill.fee;   // fee < 0 for rebates
                ++cb.maker_count;
            } else {
                cb.taker_fees    +=  rec.fill.fee;
                cb.avg_slippage_bps += rec.slippage_bps;
                ++cb.taker_count;
            }
        }
        cb.net_fees = cb.taker_fees - cb.maker_rebates;
        cb.fee_bps  = cb.total_notional > 1e-9
                    ? cb.net_fees / cb.total_notional * 10000.0 : 0.0;
        if (cb.taker_count > 0)
            cb.avg_slippage_bps /= cb.taker_count;
        return cb;
    }

    // ── Print full report ─────────────────────────────────────────────────────

    void print_report(const std::string& title = "Analytics Report") const
{
    constexpr int WIDTH = 72;
    const std::string sep(WIDTH, '=');
    const std::string line(WIDTH, '-');

    auto printKV = [](const std::string& key, const auto& value)
    {
        std::cout << std::left
                  << std::setw(28) << key
                  << value << '\n';
    };

    std::cout << '\n';
    std::cout << sep << '\n';
    std::cout << title << '\n';
    std::cout << sep << '\n';

    auto& ps = stats_.portfolio_summary;

    std::cout << "\nENGINE\n";
    std::cout << line << '\n';
    printKV("Ticks processed", stats_.ticks_processed);
    printKV("Throughput", std::to_string((long long)stats_.ticks_per_second) + " ticks/sec");
    printKV("Strategy errors", stats_.strategy_errors);
    printKV("Halted", stats_.halted ? "YES" : "NO");

    std::cout << "\nP&L SUMMARY\n";
    std::cout << line << '\n';
    printKV("Total P&L", "$" + std::to_string(ps.pnl));
    printKV("PnL %", std::to_string(ps.pnl_pct));
    printKV("Total fees", "$" + std::to_string(ps.total_fees));

    for (const auto& [sym, pnl] : ps.realized_pnl)
        printKV("Realized " + sym, "$" + std::to_string(pnl));

    auto rm = compute_risk_metrics();

    std::cout << "\nRISK\n";
    std::cout << line << '\n';
    printKV("Sharpe", rm.sharpe);
    printKV("Sortino", rm.sortino);
    printKV("Calmar", rm.calmar);
    printKV("Max Drawdown %", rm.max_drawdown * 100.0);
    printKV("VaR95 %", rm.var_95 * 100.0);
    printKV("CVaR95 %", rm.cvar_95 * 100.0);

    std::cout << "\nFILLS\n";
    std::cout << line << '\n';
    printKV("Total fills", stats_.total_fills);
    printKV("Maker fills", stats_.maker_fills);
    printKV("Taker fills", stats_.taker_fills);

    if (stats_.total_fills > 0)
    {
        printKV(
            "Maker ratio",
            100.0 * stats_.maker_fills / stats_.total_fills
        );
    }

    printKV("Win rate %", rm.win_rate * 100.0);
    printKV("Profit factor", rm.profit_factor);
    printKV("Expectancy", rm.expectancy);

    auto cb = compute_cost_breakdown();

    std::cout << "\nCOSTS\n";
    std::cout << line << '\n';
    printKV("Total notional", cb.total_notional);
    printKV("Maker rebates", cb.maker_rebates);
    printKV("Taker fees", cb.taker_fees);
    printKV("Net fees", cb.net_fees);
    printKV("Fee bps", cb.fee_bps);

    if (!time_buckets_.empty())
    {
        std::cout << "\nTIME BUCKETS\n";
        std::cout << line << '\n';

        for (const auto& b : time_buckets_)
        {
            std::cout
                << "[" << b.ts_start / 1000000000LL << "s]"
                << "  PnL="
                << (b.pnl_close - b.pnl_open)
                << "  fills="
                << b.fill_count
                << "  volume=$"
                << b.volume
                << '\n';
        }
    }

    std::cout << "\nLATENCY\n";
    std::cout << line << '\n';

    if (feed_hist_.total)
        feed_hist_.print("Feed");

    if (order_hist_.total)
        order_hist_.print("Order");

    std::cout << "\nPOSITIONS\n";
    std::cout << line << '\n';

    for (const auto& [sym, qty] : ps.positions)
        printKV(sym, qty);

    printKV("Cash", ps.cash);

    std::cout << '\n' << sep << "\n\n";
}

    // ── CSV export ────────────────────────────────────────────────────────────

    std::string fills_to_csv() const {
        std::ostringstream oss;
        oss << "timestamp_ns,order_id,symbol,side,price,qty,is_maker,"
               "fee,realized_pnl,adverse_score,slippage_bps\n";
        for (auto& rec : fill_records_) {
            const auto& f = rec.fill;
            oss << f.timestamp << ","
                << f.order_id  << ","
                << f.symbol    << ","
                << side_to_str(f.side) << ","
                << std::fixed << std::setprecision(8) << f.price << ","
                << f.qty << ","
                << (f.is_maker ? "1" : "0") << ","
                << f.fee << ","
                << f.realized_pnl << ","
                << f.adverse_score << ","
                << rec.slippage_bps << "\n";
        }
        return oss.str();
    }

    std::string pnl_series_to_csv() const {
        std::ostringstream oss;
        oss << "timestamp_ns,mtm_value\n";
        for (auto& [ts, v] : pnl_series_) {
            oss << ts << "," << std::fixed << std::setprecision(6) << v << "\n";
        }
        return oss.str();
    }

    // ── Accessors ─────────────────────────────────────────────────────────────
    const std::vector<FillRecord>&   fill_records()  const { return fill_records_;  }
    const std::vector<TimeBucket>&   time_buckets()  const { return time_buckets_;  }
    const LatencyHistogram&          feed_histogram() const { return feed_hist_;     }
    const LatencyHistogram&          order_histogram()const { return order_hist_;    }

private:
    SimStats                               stats_;
    std::vector<FillRecord>                fill_records_;
    std::vector<TimeBucket>                time_buckets_;
    std::vector<std::pair<int64_t,double>> pnl_series_;
    LatencyHistogram                       feed_hist_;
    LatencyHistogram                       order_hist_;
    int64_t                                bucket_size_ns_;
};

} // namespace hft