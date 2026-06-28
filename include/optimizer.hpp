#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// optimizer.hpp  —  Parameter optimization with walk-forward validation
//
// Supports:
//   Grid search      : exhaustive sweep over discrete parameter grids
//   Random search    : Monte Carlo sampling over parameter ranges
//   Walk-forward     : rolling in-sample/out-of-sample validation to detect overfit
//   Multi-threaded   : std::thread pool to parallelise trial runs
//   Objective        : pluggable — Sharpe, Calmar, PnL, custom lambda
//
// Walk-forward protocol (avoids look-ahead bias):
//   total data = [T0 ... TN]
//   Window:  [T0 ... T0+train] train → [T0+train ... T0+train+test] test
//   Slide:   advance by `step` each fold
//   Report:  out-of-sample aggregate metrics across all folds
// ─────────────────────────────────────────────────────────────────────────────

#include "types.hpp"
#include "simulator.hpp"
#include "portfolio.hpp"
#include "analytics.hpp"
#include <vector>
#include <string>
#include <functional>
#include <algorithm>
#include <random>
#include <thread>
#include <mutex>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <limits>
#include <map>
#include <cmath>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// ParamGrid  —  defines the search space for one trial
// ─────────────────────────────────────────────────────────────────────────────
using ParamMap = std::map<std::string, double>;

struct ParamRange {
    std::string name;
    double      lo;
    double      hi;
    double      step = 0.0;   // 0 = continuous (random search only)
};

// Build all combinations from discrete ranges (for grid search)
inline std::vector<ParamMap> build_grid(const std::vector<ParamRange>& ranges) {
    std::vector<ParamMap> result = {{}};
    for (auto& r : ranges) {
        if (r.step <= 0.0) {
            // Skip continuous ranges in grid search
            for (auto& m : result) m[r.name] = r.lo;
            continue;
        }
        std::vector<ParamMap> expanded;
        for (double v = r.lo; v <= r.hi + 1e-12; v += r.step) {
            for (auto m : result) {
                m[r.name] = v;
                expanded.push_back(m);
            }
        }
        result = std::move(expanded);
    }
    return result;
}

// Sample N random parameter sets from ranges
inline std::vector<ParamMap> sample_random(
    const std::vector<ParamRange>& ranges,
    int n_trials,
    uint64_t seed = 42)
{
    std::mt19937_64 rng(seed);
    std::vector<ParamMap> result;
    result.reserve(n_trials);
    for (int i = 0; i < n_trials; ++i) {
        ParamMap m;
        for (auto& r : ranges) {
            std::uniform_real_distribution<double> dist(r.lo, r.hi);
            m[r.name] = dist(rng);
        }
        result.push_back(std::move(m));
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// TrialResult
// ─────────────────────────────────────────────────────────────────────────────
struct TrialResult {
    ParamMap params;
    double   objective       = 0.0;
    double   sharpe          = 0.0;
    double   calmar          = 0.0;
    double   pnl             = 0.0;
    double   pnl_pct         = 0.0;
    double   max_drawdown    = 0.0;
    int64_t  total_fills     = 0;
    bool     halted          = false;
    int      fold            = -1;   // -1 = in-sample, >=0 = walk-forward fold

    bool operator>(const TrialResult& o) const { return objective > o.objective; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Objective functions
// ─────────────────────────────────────────────────────────────────────────────
inline double obj_sharpe(const TrialResult& r) {
    return r.halted ? -1e9 : r.sharpe;
}
inline double obj_calmar(const TrialResult& r) {
    return r.halted ? -1e9 : r.calmar;
}
inline double obj_pnl(const TrialResult& r) {
    return r.halted ? -1e9 : r.pnl;
}
inline double obj_sharpe_with_fillrate(const TrialResult& r) {
    if (r.halted || r.total_fills < 10) return -1e9;
    return r.sharpe * std::log(1 + r.total_fills);
}

// ─────────────────────────────────────────────────────────────────────────────
// StrategyFactory  —  caller supplies a factory that builds a Strategy + SimEngine
// ─────────────────────────────────────────────────────────────────────────────

// The factory takes a ParamMap and a data slice, runs the simulation,
// and returns a TrialResult. This keeps the optimizer generic.
using TrialFn = std::function<TrialResult(
    const ParamMap& params,
    const std::vector<MarketEvent>& data,
    int fold_id)>;

// Walk-forward configuration — defined outside Optimizer to avoid
// "default member initializer required before end of enclosing class" on GCC
struct WalkForwardConfig {
    double train_frac = 0.70;
    double test_frac  = 0.30;
    int    n_folds    = 5;
    bool   anchored   = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Optimizer
// ─────────────────────────────────────────────────────────────────────────────
class Optimizer {
public:
    struct Config {
        int      n_threads       = 1;
        int      max_trials      = 200;
        bool     verbose         = true;
        int      print_top_n     = 5;
        std::function<double(const TrialResult&)> objective;

        Config() : objective(obj_sharpe) {}
    };

    explicit Optimizer(Config cfg = Config{}) : cfg_(std::move(cfg)) {}

    // ── Grid search ───────────────────────────────────────────────────────────

    std::vector<TrialResult> grid_search(
        const std::vector<ParamRange>& ranges,
        const std::vector<MarketEvent>& data,
        TrialFn trial_fn)
    {
        auto grid = build_grid(ranges);
        if (cfg_.verbose)
            std::cout << "[Optimizer] Grid search: " << grid.size()
                      << " combinations\n";
        return run_trials(grid, data, trial_fn);
    }

    // ── Random search ─────────────────────────────────────────────────────────

    std::vector<TrialResult> random_search(
        const std::vector<ParamRange>& ranges,
        const std::vector<MarketEvent>& data,
        TrialFn trial_fn,
        int n_trials = 100,
        uint64_t seed = 42)
    {
        auto candidates = sample_random(ranges, n_trials, seed);
        if (cfg_.verbose)
            std::cout << "[Optimizer] Random search: " << n_trials << " trials\n";
        return run_trials(candidates, data, trial_fn);
    }

    // ── Walk-forward validation ───────────────────────────────────────────────

    struct WalkForwardResult {
        std::vector<TrialResult>   is_results;    // in-sample best per fold
        std::vector<TrialResult>   oos_results;   // out-of-sample results
        double                     oos_sharpe_mean = 0.0;
        double                     oos_pnl_mean    = 0.0;
        double                     oos_consistency = 0.0;  // fraction of +ve OOS folds
        ParamMap                   best_params;
    };

    WalkForwardResult walk_forward(
        const std::vector<ParamRange>& ranges,
        const std::vector<MarketEvent>& all_data,
        TrialFn trial_fn,
        WalkForwardConfig wf_cfg = WalkForwardConfig{},
        int n_random_trials = 50)
    {
        WalkForwardResult wfr;
        size_t N = all_data.size();
        size_t fold_size = N / wf_cfg.n_folds;

        if (cfg_.verbose)
            std::cout << "[WalkForward] " << wf_cfg.n_folds << " folds x "
                      << fold_size << " events/fold\n";

        // Accumulate OOS objective scores per param set
        std::map<std::string, std::pair<ParamMap, std::vector<double>>> param_oos;

        for (int fold = 0; fold < wf_cfg.n_folds; ++fold) {
            size_t fold_start, train_end, test_end;
            if (wf_cfg.anchored) {
                fold_start = 0;
                train_end  = static_cast<size_t>((fold + 1) * fold_size * wf_cfg.train_frac);
            } else {
                fold_start = static_cast<size_t>(fold * fold_size);
                train_end  = fold_start + static_cast<size_t>(fold_size * wf_cfg.train_frac);
            }
            test_end = std::min(N, fold_start + fold_size);
            if (train_end >= test_end) continue;

            // Slice data
            std::vector<MarketEvent> train_data(
                all_data.begin() + fold_start,
                all_data.begin() + train_end);
            std::vector<MarketEvent> test_data(
                all_data.begin() + train_end,
                all_data.begin() + test_end);

            if (cfg_.verbose)
                std::cout << "  Fold " << fold << ": train ["
                          << fold_start << ".." << train_end << "]  oos ["
                          << train_end << ".." << test_end << "]\n";

            // In-sample: find best params
            auto candidates = sample_random(ranges, n_random_trials, 42 + fold);
            auto is_results = run_trials(candidates, train_data, trial_fn, fold);
            if (is_results.empty()) continue;

            // Best IS params
            auto& best = is_results.front();
            wfr.is_results.push_back(best);
            if (cfg_.verbose)
                std::cout << "    Best IS: obj=" << std::setprecision(3) << best.objective
                          << "  sharpe=" << best.sharpe << "\n";

            // Out-of-sample evaluation with best IS params
            auto oos = trial_fn(best.params, test_data, -(fold + 1));
            oos.fold = fold;
            wfr.oos_results.push_back(oos);

            // Track per-param OOS history
            std::string key = params_to_key(best.params);
            param_oos[key].first = best.params;
            param_oos[key].second.push_back(cfg_.objective(oos));

            if (cfg_.verbose)
                std::cout << "    OOS:    obj=" << cfg_.objective(oos)
                          << "  pnl=$" << std::setprecision(4) << oos.pnl
                          << "  sharpe=" << std::setprecision(3) << oos.sharpe << "\n";
        }

        // Compute OOS aggregate metrics
        if (!wfr.oos_results.empty()) {
            double sum_sharpe = 0.0, sum_pnl = 0.0;
            int positive = 0;
            for (auto& r : wfr.oos_results) {
                sum_sharpe += r.sharpe;
                sum_pnl    += r.pnl;
                if (r.pnl > 0) ++positive;
            }
            int n = static_cast<int>(wfr.oos_results.size());
            wfr.oos_sharpe_mean  = sum_sharpe / n;
            wfr.oos_pnl_mean     = sum_pnl / n;
            wfr.oos_consistency  = static_cast<double>(positive) / n;
        }

        // Best params: use IS results with highest average OOS score
        double best_oos_avg = -1e18;
        for (auto& [key, kv] : param_oos) {
            double avg = 0.0;
            for (double v : kv.second) avg += v;
            avg /= kv.second.size();
            if (avg > best_oos_avg) {
                best_oos_avg  = avg;
                wfr.best_params = kv.first;
            }
        }

        if (cfg_.verbose) {
            std::cout << "\n[WalkForward] Summary:\n";
            std::cout << "  OOS Sharpe mean  : " << wfr.oos_sharpe_mean << "\n";
            std::cout << "  OOS PnL mean     : $" << wfr.oos_pnl_mean << "\n";
            std::cout << "  OOS Consistency  : "
                      << std::setprecision(1) << wfr.oos_consistency * 100 << "%\n";
            std::cout << "  Best params      : ";
            for (auto& [k, v] : wfr.best_params)
                std::cout << k << "=" << std::setprecision(4) << v << " ";
            std::cout << "\n";
        }

        return wfr;
    }

    const Config& config() const { return cfg_; }

private:
    Config cfg_;

    // ── Run a list of param sets in parallel ──────────────────────────────────

    std::vector<TrialResult> run_trials(
        const std::vector<ParamMap>& candidates,
        const std::vector<MarketEvent>& data,
        TrialFn trial_fn,
        int fold_id = -1)
    {
        std::vector<TrialResult> results(candidates.size());
        std::atomic<int> idx{0};
        std::atomic<int> done{0};
        std::mutex print_mu;

        int n_threads = std::max(1, std::min(cfg_.n_threads,
                                 static_cast<int>(candidates.size())));

        auto worker = [&]() {
            while (true) {
                int i = idx.fetch_add(1);
                if (i >= static_cast<int>(candidates.size())) break;

                TrialResult r = trial_fn(candidates[i], data, fold_id);
                r.objective   = cfg_.objective(r);
                r.params      = candidates[i];
                r.fold        = fold_id;
                results[i]    = std::move(r);

                int d = done.fetch_add(1) + 1;
                if (cfg_.verbose && d % std::max(1, (int)candidates.size()/20) == 0) {
                    std::lock_guard<std::mutex> lock(print_mu);
                    std::cout << "  [" << d << "/" << candidates.size()
                              << "] best so far: obj=" << std::setprecision(3)
                              << results[i].objective << "        \n";
                }
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(n_threads);
        for (int t = 0; t < n_threads; ++t)
            threads.emplace_back(worker);
        for (auto& t : threads) t.join();

        if (cfg_.verbose) std::cout << "\n";

        // Sort by objective descending
        std::sort(results.begin(), results.end(), std::greater<TrialResult>());

        // Print top N
        if (cfg_.verbose && cfg_.print_top_n > 0) {
            int top = std::min((int)cfg_.print_top_n, (int)results.size());
            std::cout << "  Top " << top << " results:\n";
            for (int i = 0; i < top; ++i) {
                auto& r = results[i];
                std::cout << "    [" << i+1 << "] obj=" << std::setprecision(3)
                          << r.objective
                          << "  sharpe=" << r.sharpe
                          << "  pnl=$"   << std::setprecision(4) << r.pnl
                          << "  dd="     << std::setprecision(2)
                          << r.max_drawdown * 100 << "%"
                          << "  params=";
                for (auto& [k, v] : r.params)
                    std::cout << k << "=" << std::setprecision(3) << v << " ";
                std::cout << "\n";
            }
        }
        return results;
    }

    static std::string params_to_key(const ParamMap& p) {
        std::ostringstream oss;
        for (auto& [k, v] : p) oss << k << "=" << std::fixed << std::setprecision(4) << v << ";";
        return oss.str();
    }
};

} // namespace hft