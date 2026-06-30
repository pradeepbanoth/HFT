#pragma once
// tickdb/replay.hpp — advanced deterministic / realtime / accelerated replay engine

#include "tickdb/query.hpp"
#include "simulator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace hft::tickdb {

enum class ReplayMode : uint8_t {
    Deterministic,
    Realtime,
    Accelerated,
    Batch,
    Step
};

enum class ReplayState : uint8_t {
    Created,
    Loaded,
    Running,
    Paused,
    Completed,
    Cancelled,
    Error
};

struct ReplayConfig {
    ReplayMode mode = ReplayMode::Deterministic;

    double speed = 1.0;
    size_t batch_size = 1000;
    size_t max_events = 0;

    bool sort_events = true;
    bool stop_on_callback_false = true;
    bool preserve_query_order = false;

    int64_t start_delay_ns = 0;
    int64_t max_sleep_ns = 100'000'000LL;
};

struct ReplayCheckpoint {
    size_t index = 0;
    int64_t current_ts = 0;
    uint64_t events_replayed = 0;
};

struct ReplayStats {
    uint64_t events_loaded = 0;
    uint64_t events_replayed = 0;
    uint64_t batches = 0;

    uint64_t callback_false_stops = 0;
    uint64_t pauses = 0;
    uint64_t seeks = 0;

    int64_t first_ts = 0;
    int64_t last_ts = 0;
    int64_t current_ts = 0;

    double wall_time_s = 0.0;
    double events_per_second = 0.0;

    double replay_time_s() const noexcept {
        if (last_ts <= first_ts) return 0.0;
        return static_cast<double>(last_ts - first_ts) / 1e9;
    }
};

struct ReplayHooks {
    std::function<void()> on_start;
    std::function<void()> on_pause;
    std::function<void()> on_resume;
    std::function<void()> on_complete;
    std::function<void(const std::string&)> on_error;
};

using ReplayCallback = std::function<bool(const MarketEvent&)>;
using ReplayBatchCallback = std::function<bool(const std::vector<MarketEvent>&)>;

class ReplayClock {
public:
    void reset(int64_t first_ts) {
        first_market_ts_ = first_ts;
        wall_start_ = std::chrono::high_resolution_clock::now();
    }

    int64_t current_replay_ns(double speed = 1.0) const {
        auto now = std::chrono::high_resolution_clock::now();
        auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - wall_start_).count();
        return first_market_ts_ + static_cast<int64_t>(static_cast<double>(wall_ns) * speed);
    }

private:
    int64_t first_market_ts_ = 0;
    std::chrono::high_resolution_clock::time_point wall_start_;
};

class ReplayEngine {
public:
    ReplayEngine(const MetadataCatalog& catalog, ReplayConfig cfg = {})
        : query_engine_(catalog)
        , cfg_(cfg)
    {}

    void set_hooks(ReplayHooks hooks) {
        hooks_ = std::move(hooks);
    }

    QueryResult load(const TickQuery& q) {
        reset_runtime();

        auto result = query_engine_.execute(q);
        events_ = std::move(result.events);

        if (cfg_.sort_events && !cfg_.preserve_query_order) {
            std::stable_sort(events_.begin(), events_.end(),
                [](const MarketEvent& a, const MarketEvent& b) {
                    return event_timestamp(a) < event_timestamp(b);
                });
        }

        if (cfg_.max_events > 0 && events_.size() > cfg_.max_events)
            events_.resize(cfg_.max_events);

        stats_.events_loaded = events_.size();

        if (!events_.empty()) {
            stats_.first_ts = event_timestamp(events_.front());
            stats_.last_ts = event_timestamp(events_.back());
            stats_.current_ts = stats_.first_ts;
        }

        state_.store(ReplayState::Loaded);
        return result;
    }

    bool load_events(std::vector<MarketEvent> events) {
        reset_runtime();
        events_ = std::move(events);

        if (cfg_.sort_events) {
            std::stable_sort(events_.begin(), events_.end(),
                [](const MarketEvent& a, const MarketEvent& b) {
                    return event_timestamp(a) < event_timestamp(b);
                });
        }

        if (cfg_.max_events > 0 && events_.size() > cfg_.max_events)
            events_.resize(cfg_.max_events);

        stats_.events_loaded = events_.size();

        if (!events_.empty()) {
            stats_.first_ts = event_timestamp(events_.front());
            stats_.last_ts = event_timestamp(events_.back());
            stats_.current_ts = stats_.first_ts;
        }

        state_.store(ReplayState::Loaded);
        return !events_.empty();
    }

    bool step(ReplayCallback cb) {
        if (idx_ >= events_.size()) {
            state_.store(ReplayState::Completed);
            return false;
        }

        state_.store(ReplayState::Running);

        const MarketEvent& evt = events_[idx_];
        stats_.current_ts = event_timestamp(evt);

        bool ok = cb ? cb(evt) : true;

        ++idx_;
        ++stats_.events_replayed;

        if (!ok && cfg_.stop_on_callback_false) {
            ++stats_.callback_false_stops;
            state_.store(ReplayState::Cancelled);
            return false;
        }

        if (idx_ >= events_.size()) {
            state_.store(ReplayState::Completed);
            if (hooks_.on_complete) hooks_.on_complete();
        }

        return ok;
    }

    ReplayStats run(ReplayCallback cb) {
        auto t0 = std::chrono::high_resolution_clock::now();

        if (events_.empty()) {
            state_.store(ReplayState::Completed);
            finish_timer(t0);
            return stats_;
        }

        state_.store(ReplayState::Running);
        clock_.reset(event_timestamp(events_.front()));

        if (cfg_.start_delay_ns > 0)
            std::this_thread::sleep_for(std::chrono::nanoseconds(cfg_.start_delay_ns));

        if (hooks_.on_start) hooks_.on_start();

        try {
            if (cfg_.mode == ReplayMode::Batch) {
                run_batch([&](const std::vector<MarketEvent>& batch) {
                    for (const auto& evt : batch) {
                        if (cb && !cb(evt)) return false;
                    }
                    return true;
                });
            } else {
                while (idx_ < events_.size()) {
                    wait_if_paused();

                    auto st = state_.load();
                    if (st == ReplayState::Cancelled || st == ReplayState::Error) break;

                    if (cfg_.mode == ReplayMode::Realtime || cfg_.mode == ReplayMode::Accelerated)
                        sleep_until_next();

                    if (!step(cb)) break;
                }
            }
        } catch (const std::exception& e) {
            state_.store(ReplayState::Error);
            if (hooks_.on_error) hooks_.on_error(e.what());
        } catch (...) {
            state_.store(ReplayState::Error);
            if (hooks_.on_error) hooks_.on_error("unknown_replay_error");
        }

        finish_timer(t0);
        return stats_;
    }

    ReplayStats run_batch(ReplayBatchCallback cb) {
        auto t0 = std::chrono::high_resolution_clock::now();

        if (events_.empty()) {
            state_.store(ReplayState::Completed);
            finish_timer(t0);
            return stats_;
        }

        state_.store(ReplayState::Running);
        if (hooks_.on_start) hooks_.on_start();

        std::vector<MarketEvent> batch;
        batch.reserve(std::max<size_t>(1, cfg_.batch_size));

        while (idx_ < events_.size()) {
            wait_if_paused();

            auto st = state_.load();
            if (st == ReplayState::Cancelled || st == ReplayState::Error) break;

            batch.clear();

            size_t end = std::min(events_.size(), idx_ + std::max<size_t>(1, cfg_.batch_size));
            for (; idx_ < end; ++idx_) {
                batch.push_back(events_[idx_]);
                stats_.current_ts = event_timestamp(events_[idx_]);
            }

            ++stats_.batches;
            stats_.events_replayed += batch.size();

            bool ok = cb ? cb(batch) : true;
            if (!ok && cfg_.stop_on_callback_false) {
                ++stats_.callback_false_stops;
                state_.store(ReplayState::Cancelled);
                break;
            }
        }

        if (idx_ >= events_.size() && state_.load() == ReplayState::Running) {
            state_.store(ReplayState::Completed);
            if (hooks_.on_complete) hooks_.on_complete();
        }

        finish_timer(t0);
        return stats_;
    }

    SimStats run_into_engine(SimEngine& engine) {
        engine.on_start_manual();

        run([&](const MarketEvent& evt) {
            engine.process_one(evt);
            return true;
        });

        return engine.on_end_manual();
    }

    bool seek_index(size_t idx) {
        if (idx > events_.size()) return false;
        idx_ = idx;
        ++stats_.seeks;

        if (idx_ < events_.size())
            stats_.current_ts = event_timestamp(events_[idx_]);

        return true;
    }

    bool seek_time(int64_t ts) {
        auto it = std::lower_bound(events_.begin(), events_.end(), ts,
            [](const MarketEvent& e, int64_t t) {
                return event_timestamp(e) < t;
            });

        idx_ = static_cast<size_t>(std::distance(events_.begin(), it));
        ++stats_.seeks;

        if (idx_ < events_.size())
            stats_.current_ts = event_timestamp(events_[idx_]);

        return idx_ < events_.size();
    }

    ReplayCheckpoint checkpoint() const {
        ReplayCheckpoint cp;
        cp.index = idx_;
        cp.current_ts = stats_.current_ts;
        cp.events_replayed = stats_.events_replayed;
        return cp;
    }

    bool restore(const ReplayCheckpoint& cp) {
        if (cp.index > events_.size()) return false;
        idx_ = cp.index;
        stats_.current_ts = cp.current_ts;
        stats_.events_replayed = cp.events_replayed;
        return true;
    }

    void pause() {
        if (state_.load() == ReplayState::Running) {
            state_.store(ReplayState::Paused);
            ++stats_.pauses;
            if (hooks_.on_pause) hooks_.on_pause();
        }
    }

    void resume() {
        if (state_.load() == ReplayState::Paused) {
            state_.store(ReplayState::Running);
            if (hooks_.on_resume) hooks_.on_resume();
        }
    }

    void cancel() {
        state_.store(ReplayState::Cancelled);
    }

    void set_speed(double speed) {
        cfg_.speed = std::max(1e-9, speed);
    }

    ReplayState state() const noexcept {
        return state_.load();
    }

    const ReplayStats& stats() const noexcept {
        return stats_;
    }

    size_t remaining() const noexcept {
        return idx_ >= events_.size() ? 0 : events_.size() - idx_;
    }

    size_t position() const noexcept {
        return idx_;
    }

    const std::vector<MarketEvent>& events() const noexcept {
        return events_;
    }

private:
    TickQueryEngine query_engine_;
    ReplayConfig cfg_;
    ReplayHooks hooks_;
    ReplayClock clock_;

    std::vector<MarketEvent> events_;
    size_t idx_ = 0;

    std::atomic<ReplayState> state_{ReplayState::Created};
    ReplayStats stats_;

    void reset_runtime() {
        events_.clear();
        idx_ = 0;
        stats_ = ReplayStats{};
        state_.store(ReplayState::Created);
    }

    void wait_if_paused() {
        while (state_.load() == ReplayState::Paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void sleep_until_next() {
        if (idx_ == 0 || idx_ >= events_.size()) return;

        int64_t prev_ts = event_timestamp(events_[idx_ - 1]);
        int64_t cur_ts = event_timestamp(events_[idx_]);

        int64_t delta_ns = std::max<int64_t>(0, cur_ts - prev_ts);

        double speed = cfg_.mode == ReplayMode::Accelerated
            ? std::max(1e-9, cfg_.speed)
            : 1.0;

        int64_t sleep_ns = static_cast<int64_t>(static_cast<double>(delta_ns) / speed);

        if (cfg_.max_sleep_ns > 0)
            sleep_ns = std::min<int64_t>(sleep_ns, cfg_.max_sleep_ns);

        if (sleep_ns > 0)
            std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
    }

    void finish_timer(std::chrono::high_resolution_clock::time_point t0) {
        auto t1 = std::chrono::high_resolution_clock::now();

        stats_.wall_time_s = std::chrono::duration<double>(t1 - t0).count();
        stats_.events_per_second = stats_.wall_time_s > 0.0
            ? static_cast<double>(stats_.events_replayed) / stats_.wall_time_s
            : 0.0;

        if (idx_ >= events_.size() && state_.load() == ReplayState::Running) {
            state_.store(ReplayState::Completed);
            if (hooks_.on_complete) hooks_.on_complete();
        }
    }
};

} // namespace hft::tickdb