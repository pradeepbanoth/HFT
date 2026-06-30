#pragma once
// distributed_optimizer.hpp — advanced distributed/local-parallel optimizer

#include "optimizer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hft {

enum class DistTrialStatus : uint8_t {
    Pending,
    Running,
    Completed,
    Failed,
    Retrying,
    Skipped
};

struct WorkerTelemetry {
    int worker_id = -1;
    int64_t started = 0;
    int64_t completed = 0;
    int64_t failed = 0;
    double avg_trial_ms = 0.0;
    double last_trial_ms = 0.0;
};

struct DistTrial {
    int64_t id = 0;
    std::string key;
    ParamMap params;
    DistTrialStatus status = DistTrialStatus::Pending;
    TrialResult result;
    std::string error;
    int retries = 0;
    int worker_id = -1;
    int64_t started_ns = 0;
    int64_t finished_ns = 0;
};

struct DistOptConfig {
    int n_workers = std::max(1u, std::thread::hardware_concurrency());
    int max_retries = 1;
    int top_k = 20;
    int checkpoint_every = 10;

    bool verbose = true;
    bool resume = true;
    bool checkpoint = true;

    std::string checkpoint_path = "dist_optimizer_checkpoint.csv";

    std::function<double(const TrialResult&)> objective = obj_sharpe;
};

struct DistOptSummary {
    int64_t total = 0;
    int64_t completed = 0;
    int64_t failed = 0;
    int64_t skipped = 0;
    double wall_time_s = 0.0;
    double trials_per_second = 0.0;
    TrialResult best;
    std::vector<TrialResult> top;
    std::vector<WorkerTelemetry> workers;
};

class DistributedOptimizer {
public:
    using TrialFn = std::function<TrialResult(
        const ParamMap&,
        const std::vector<MarketEvent>&,
        int)>;

    explicit DistributedOptimizer(DistOptConfig cfg = {})
        : cfg_(std::move(cfg)) {}

    DistOptSummary run(
        std::vector<ParamMap> params,
        const std::vector<MarketEvent>& data,
        TrialFn fn,
        int fold_id = -1
    ) {
        prepare(std::move(params));

        if (cfg_.resume) load_checkpoint();

        workers_.clear();
        workers_.resize(std::max(1, cfg_.n_workers));
        for (int i = 0; i < static_cast<int>(workers_.size()); ++i)
            workers_[i].worker_id = i;

        auto t0 = std::chrono::high_resolution_clock::now();

        std::atomic<int64_t> cursor{0};
        std::atomic<int64_t> progress{0};

        auto worker_fn = [&](int wid) {
            while (true) {
                int64_t i = cursor.fetch_add(1);
                if (i >= static_cast<int64_t>(trials_.size())) break;

                run_one(wid, trials_[i], data, fn, fold_id);

                int64_t p = ++progress;

                if (cfg_.verbose && p % std::max(1, cfg_.checkpoint_every) == 0) {
                    std::lock_guard<std::mutex> lock(mu_);
                    print_progress_unlocked();
                }

                if (cfg_.checkpoint && p % std::max(1, cfg_.checkpoint_every) == 0) {
                    std::lock_guard<std::mutex> lock(mu_);
                    save_checkpoint_unlocked();
                }
            }
        };

        std::vector<std::thread> pool;
        for (int i = 0; i < std::max(1, cfg_.n_workers); ++i)
            pool.emplace_back(worker_fn, i);

        for (auto& t : pool) t.join();

        {
            std::lock_guard<std::mutex> lock(mu_);
            if (cfg_.checkpoint) save_checkpoint_unlocked();
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        double wall = std::chrono::duration<double>(t1 - t0).count();

        return summary(wall);
    }

    DistOptSummary grid_search(
        const std::vector<ParamRange>& ranges,
        const std::vector<MarketEvent>& data,
        TrialFn fn,
        int fold_id = -1
    ) {
        return run(build_grid(ranges), data, std::move(fn), fold_id);
    }

    DistOptSummary random_search(
        const std::vector<ParamRange>& ranges,
        int n,
        const std::vector<MarketEvent>& data,
        TrialFn fn,
        uint64_t seed = 42,
        int fold_id = -1
    ) {
        return run(sample_random(ranges, n, seed), data, std::move(fn), fold_id);
    }

    std::vector<TrialResult> monte_carlo_robustness(
        const ParamMap& base,
        int n_trials,
        double noise_frac,
        const std::vector<MarketEvent>& data,
        TrialFn fn,
        uint64_t seed = 123
    ) {
        std::mt19937_64 rng(seed);
        std::vector<ParamMap> perturbed;
        perturbed.reserve(n_trials);

        for (int i = 0; i < n_trials; ++i) {
            ParamMap p = base;
            for (auto& [k, v] : p) {
                double width = std::abs(v) * noise_frac;
                std::uniform_real_distribution<double> d(v - width, v + width);
                p[k] = d(rng);
            }
            perturbed.push_back(std::move(p));
        }

        auto s = run(std::move(perturbed), data, std::move(fn), -1);
        return s.top;
    }

    std::string results_to_csv() const {
        std::ostringstream oss;
        oss << "objective,sharpe,calmar,pnl,pnl_pct,max_drawdown,total_fills,params\n";

        std::vector<TrialResult> copy;
        {
            std::lock_guard<std::mutex> lock(mu_);
            copy = results_;
        }

        std::sort(copy.begin(), copy.end(), [](const TrialResult& a, const TrialResult& b) {
            return a.objective > b.objective;
        });

        for (const auto& r : copy) {
            oss << r.objective << ","
                << r.sharpe << ","
                << r.calmar << ","
                << r.pnl << ","
                << r.pnl_pct << ","
                << r.max_drawdown << ","
                << r.total_fills << ","
                << quote(params_key(r.params)) << "\n";
        }

        return oss.str();
    }

    const std::vector<DistTrial>& trials() const noexcept { return trials_; }

private:
    DistOptConfig cfg_;
    std::vector<DistTrial> trials_;
    std::vector<TrialResult> results_;
    std::vector<WorkerTelemetry> workers_;

    std::unordered_set<std::string> completed_keys_;
    mutable std::mutex mu_;

    int64_t completed_ = 0;
    int64_t failed_ = 0;
    int64_t skipped_ = 0;

    static int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    static std::string params_key(const ParamMap& p) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(12);
        for (const auto& [k, v] : p)
            oss << k << "=" << v << ";";
        return oss.str();
    }

    static std::string quote(const std::string& s) {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"') out += "\"\"";
            else out += c;
        }
        out += "\"";
        return out;
    }

    static const char* status_to_str(DistTrialStatus s) {
        switch (s) {
            case DistTrialStatus::Pending: return "pending";
            case DistTrialStatus::Running: return "running";
            case DistTrialStatus::Completed: return "completed";
            case DistTrialStatus::Failed: return "failed";
            case DistTrialStatus::Retrying: return "retrying";
            case DistTrialStatus::Skipped: return "skipped";
            default: return "unknown";
        }
    }

    void prepare(std::vector<ParamMap> params) {
        trials_.clear();
        results_.clear();
        completed_keys_.clear();
        completed_ = failed_ = skipped_ = 0;

        int64_t id = 0;
        for (auto& p : params) {
            DistTrial t;
            t.id = ++id;
            t.params = std::move(p);
            t.key = params_key(t.params);
            trials_.push_back(std::move(t));
        }
    }

    void run_one(
        int wid,
        DistTrial& trial,
        const std::vector<MarketEvent>& data,
        TrialFn& fn,
        int fold_id
    ) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (completed_keys_.count(trial.key)) {
                trial.status = DistTrialStatus::Skipped;
                ++skipped_;
                return;
            }

            trial.status = DistTrialStatus::Running;
            trial.worker_id = wid;
            trial.started_ns = now_ns();
            workers_[wid].started++;
        }

        while (trial.retries <= cfg_.max_retries) {
            auto t0 = std::chrono::high_resolution_clock::now();

            try {
                TrialResult r = fn(trial.params, data, fold_id);
                r.params = trial.params;
                r.fold = fold_id;
                r.objective = cfg_.objective(r);

                auto t1 = std::chrono::high_resolution_clock::now();
                double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

                std::lock_guard<std::mutex> lock(mu_);
                trial.result = r;
                trial.status = DistTrialStatus::Completed;
                trial.finished_ns = now_ns();
                completed_keys_.insert(trial.key);
                results_.push_back(std::move(r));
                ++completed_;

                workers_[wid].completed++;
                workers_[wid].last_trial_ms = ms;
                workers_[wid].avg_trial_ms =
                    workers_[wid].avg_trial_ms * 0.95 + ms * 0.05;

                return;
            } catch (const std::exception& e) {
                trial.error = e.what();
            } catch (...) {
                trial.error = "unknown_exception";
            }

            ++trial.retries;

            if (trial.retries <= cfg_.max_retries) {
                std::lock_guard<std::mutex> lock(mu_);
                trial.status = DistTrialStatus::Retrying;
            }
        }

        std::lock_guard<std::mutex> lock(mu_);
        trial.status = DistTrialStatus::Failed;
        trial.finished_ns = now_ns();
        ++failed_;
        workers_[wid].failed++;
    }

    void load_checkpoint() {
        std::ifstream in(cfg_.checkpoint_path);
        if (!in) return;

        std::string line;
        std::getline(in, line);

        while (std::getline(in, line)) {
            auto key = extract_second_csv_field(line);
            if (!key.empty())
                completed_keys_.insert(key);
        }

        if (cfg_.verbose && !completed_keys_.empty()) {
            std::cout << "[DistOpt] resume: " << completed_keys_.size()
                      << " completed trials found\n";
        }
    }

    static std::string extract_second_csv_field(const std::string& line) {
        int field = 0;
        bool quoted = false;
        std::string cur;

        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];

            if (c == '"') {
                if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
                    if (field == 1) cur += '"';
                    ++i;
                } else {
                    quoted = !quoted;
                }
                continue;
            }

            if (c == ',' && !quoted) {
                if (field == 1) return cur;
                ++field;
                cur.clear();
                continue;
            }

            if (field == 1) cur += c;
        }

        return field == 1 ? cur : "";
    }

    void save_checkpoint_unlocked() const {
        std::ofstream out(cfg_.checkpoint_path, std::ios::trunc);
        if (!out) return;

        out << "trial_id,param_key,status,objective,sharpe,calmar,pnl,pnl_pct,dd,fills,worker,retries,error\n";

        for (const auto& t : trials_) {
            if (t.status != DistTrialStatus::Completed &&
                t.status != DistTrialStatus::Failed &&
                t.status != DistTrialStatus::Skipped)
                continue;

            out << t.id << ","
                << quote(t.key) << ","
                << status_to_str(t.status) << ","
                << t.result.objective << ","
                << t.result.sharpe << ","
                << t.result.calmar << ","
                << t.result.pnl << ","
                << t.result.pnl_pct << ","
                << t.result.max_drawdown << ","
                << t.result.total_fills << ","
                << t.worker_id << ","
                << t.retries << ","
                << quote(t.error) << "\n";
        }
    }

    void print_progress_unlocked() const {
        int64_t done = completed_ + failed_ + skipped_;
        int64_t total = static_cast<int64_t>(trials_.size());

        double best = results_.empty()
            ? 0.0
            : std::max_element(results_.begin(), results_.end(),
                [](const TrialResult& a, const TrialResult& b) {
                    return a.objective < b.objective;
                })->objective;

        std::cout << "[DistOpt] "
                  << done << "/" << total
                  << " completed=" << completed_
                  << " failed=" << failed_
                  << " skipped=" << skipped_
                  << " best=" << std::setprecision(5) << best
                  << "\n";
    }

    DistOptSummary summary(double wall) {
        std::sort(results_.begin(), results_.end(),
            [](const TrialResult& a, const TrialResult& b) {
                return a.objective > b.objective;
            });

        DistOptSummary s;
        s.total = static_cast<int64_t>(trials_.size());
        s.completed = completed_;
        s.failed = failed_;
        s.skipped = skipped_;
        s.wall_time_s = wall;
        s.trials_per_second =
            wall > 0.0 ? static_cast<double>(completed_ + failed_) / wall : 0.0;

        if (!results_.empty()) s.best = results_.front();

        int k = std::min<int>(cfg_.top_k, results_.size());
        s.top.assign(results_.begin(), results_.begin() + k);
        s.workers = workers_;

        if (cfg_.verbose) {
            std::cout << "\n[DistOpt] Summary\n";
            std::cout << "  total      : " << s.total << "\n";
            std::cout << "  completed  : " << s.completed << "\n";
            std::cout << "  failed     : " << s.failed << "\n";
            std::cout << "  skipped    : " << s.skipped << "\n";
            std::cout << "  wall       : " << std::fixed << std::setprecision(3)
                      << s.wall_time_s << "s\n";
            std::cout << "  trials/sec : " << s.trials_per_second << "\n";
            if (!results_.empty()) {
                std::cout << "  best obj   : " << s.best.objective << "\n";
                std::cout << "  best pnl   : $" << s.best.pnl << "\n";
                std::cout << "  best sharpe: " << s.best.sharpe << "\n";
            }
        }

        return s;
    }
};

} // namespace hft