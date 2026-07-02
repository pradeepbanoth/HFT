#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hft::exchange {

enum class DispatcherState : uint8_t {
    Created,
    Running,
    Paused,
    Stopping,
    Stopped,
    Failed
};

struct DispatcherConfig {
    std::size_t worker_count{1};
    std::size_t batch_size{512};

    std::chrono::microseconds idle_sleep{25};
    std::chrono::milliseconds stop_drain_timeout{2000};

    bool drain_on_stop{true};
    bool yield_on_empty{true};
    bool enable_exception_capture{true};
};

struct DispatcherWorkerStats {
    std::atomic<uint64_t> loops{0};
    std::atomic<uint64_t> batches{0};
    std::atomic<uint64_t> events{0};
    std::atomic<uint64_t> empty_polls{0};
    std::atomic<uint64_t> exceptions{0};
    std::atomic<uint64_t> last_active_ns{0};
};

struct DispatcherStats {
    std::atomic<uint64_t> total_loops{0};
    std::atomic<uint64_t> total_batches{0};
    std::atomic<uint64_t> total_events{0};
    std::atomic<uint64_t> empty_polls{0};
    std::atomic<uint64_t> exceptions{0};
    std::atomic<uint64_t> starts{0};
    std::atomic<uint64_t> stops{0};
};

inline uint64_t dispatcher_now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

template <typename Bus>
class ExchangeDispatcher {
public:
    using AffinityHook = std::function<void(std::size_t)>;
    using ErrorHook = std::function<void(std::size_t, const std::exception&)>;

    explicit ExchangeDispatcher(
        Bus& bus,
        DispatcherConfig config = {},
        AffinityHook affinity_hook = {},
        ErrorHook error_hook = {}
    )
        : bus_(bus),
          config_(normalize(config)),
          affinity_hook_(std::move(affinity_hook)),
          error_hook_(std::move(error_hook)),
          worker_stats_(config_.worker_count) {}

    ExchangeDispatcher(const ExchangeDispatcher&) = delete;
    ExchangeDispatcher& operator=(const ExchangeDispatcher&) = delete;

    ~ExchangeDispatcher() {
        stop();
    }

    bool start() {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);

        auto current = state_.load(std::memory_order_acquire);

        if (current == DispatcherState::Running ||
            current == DispatcherState::Paused ||
            current == DispatcherState::Stopping) {
            return false;
        }

        stop_requested_.store(false, std::memory_order_release);
        pause_requested_.store(false, std::memory_order_release);

        workers_.clear();
        workers_.reserve(config_.worker_count);

        state_.store(DispatcherState::Running, std::memory_order_release);
        stats_.starts.fetch_add(1, std::memory_order_relaxed);

        try {
            for (std::size_t i = 0; i < config_.worker_count; ++i) {
                workers_.emplace_back([this, i] {
                    worker_loop(i);
                });
            }
        } catch (...) {
            stop_requested_.store(true, std::memory_order_release);
            state_.store(DispatcherState::Failed, std::memory_order_release);
            return false;
        }

        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);

            auto current = state_.load(std::memory_order_acquire);

            if (current == DispatcherState::Created ||
                current == DispatcherState::Stopped) {
                return;
            }

            state_.store(DispatcherState::Stopping, std::memory_order_release);
            stop_requested_.store(true, std::memory_order_release);
            pause_requested_.store(false, std::memory_order_release);
        }

        cv_.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        workers_.clear();

        if (config_.drain_on_stop) {
            drain_remaining();
        }

        stats_.stops.fetch_add(1, std::memory_order_relaxed);
        state_.store(DispatcherState::Stopped, std::memory_order_release);
    }

    bool pause() {
        auto expected = DispatcherState::Running;

        if (!state_.compare_exchange_strong(
                expected,
                DispatcherState::Paused,
                std::memory_order_acq_rel
            )) {
            return false;
        }

        pause_requested_.store(true, std::memory_order_release);
        return true;
    }

    bool resume() {
        auto expected = DispatcherState::Paused;

        if (!state_.compare_exchange_strong(
                expected,
                DispatcherState::Running,
                std::memory_order_acq_rel
            )) {
            return false;
        }

        pause_requested_.store(false, std::memory_order_release);
        cv_.notify_all();
        return true;
    }

    bool running() const noexcept {
        return state_.load(std::memory_order_acquire) == DispatcherState::Running;
    }

    bool paused() const noexcept {
        return state_.load(std::memory_order_acquire) == DispatcherState::Paused;
    }

    DispatcherState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    const DispatcherStats& stats() const noexcept {
        return stats_;
    }

    const DispatcherWorkerStats& worker_stats(std::size_t worker_id) const {
        return worker_stats_.at(worker_id);
    }

    std::size_t worker_count() const noexcept {
        return config_.worker_count;
    }

private:
    static DispatcherConfig normalize(DispatcherConfig config) {
        if (config.worker_count == 0) config.worker_count = 1;
        if (config.batch_size == 0) config.batch_size = 1;
        if (config.idle_sleep.count() < 0) {
            config.idle_sleep = std::chrono::microseconds(25);
        }
        return config;
    }

    void worker_loop(std::size_t worker_id) {
        if (affinity_hook_) {
            affinity_hook_(worker_id);
        }

        auto& local = worker_stats_[worker_id];

        while (!stop_requested_.load(std::memory_order_acquire)) {
            wait_if_paused();

            if (stop_requested_.load(std::memory_order_acquire)) {
                break;
            }

            try {
                local.loops.fetch_add(1, std::memory_order_relaxed);
                stats_.total_loops.fetch_add(1, std::memory_order_relaxed);

                const auto dispatched = bus_.dispatch_once(config_.batch_size);

                if (dispatched > 0) {
                    local.batches.fetch_add(1, std::memory_order_relaxed);
                    local.events.fetch_add(dispatched, std::memory_order_relaxed);
                    local.last_active_ns.store(dispatcher_now_ns(), std::memory_order_relaxed);

                    stats_.total_batches.fetch_add(1, std::memory_order_relaxed);
                    stats_.total_events.fetch_add(dispatched, std::memory_order_relaxed);

                    continue;
                }

                local.empty_polls.fetch_add(1, std::memory_order_relaxed);
                stats_.empty_polls.fetch_add(1, std::memory_order_relaxed);

                idle_backoff();

            } catch (const std::exception& e) {
                local.exceptions.fetch_add(1, std::memory_order_relaxed);
                stats_.exceptions.fetch_add(1, std::memory_order_relaxed);

                if (error_hook_) {
                    error_hook_(worker_id, e);
                }

                if (!config_.enable_exception_capture) {
                    state_.store(DispatcherState::Failed, std::memory_order_release);
                    stop_requested_.store(true, std::memory_order_release);
                    break;
                }
            } catch (...) {
                local.exceptions.fetch_add(1, std::memory_order_relaxed);
                stats_.exceptions.fetch_add(1, std::memory_order_relaxed);

                if (!config_.enable_exception_capture) {
                    state_.store(DispatcherState::Failed, std::memory_order_release);
                    stop_requested_.store(true, std::memory_order_release);
                    break;
                }
            }
        }

        if (config_.drain_on_stop) {
            drain_worker_tail(worker_id);
        }
    }

    void wait_if_paused() {
        if (!pause_requested_.load(std::memory_order_acquire)) {
            return;
        }

        std::unique_lock<std::mutex> lock(cv_mutex_);

        cv_.wait(lock, [&] {
            return !pause_requested_.load(std::memory_order_acquire) ||
                   stop_requested_.load(std::memory_order_acquire);
        });
    }

    void idle_backoff() {
        if (config_.yield_on_empty) {
            std::this_thread::yield();
        }

        if (config_.idle_sleep.count() > 0) {
            std::this_thread::sleep_for(config_.idle_sleep);
        }
    }

    void drain_worker_tail(std::size_t worker_id) {
        auto& local = worker_stats_[worker_id];

        const auto deadline =
            std::chrono::steady_clock::now() + config_.stop_drain_timeout;

        while (std::chrono::steady_clock::now() < deadline) {
            const auto dispatched = bus_.dispatch_once(config_.batch_size);

            if (dispatched == 0) {
                break;
            }

            local.batches.fetch_add(1, std::memory_order_relaxed);
            local.events.fetch_add(dispatched, std::memory_order_relaxed);
            local.last_active_ns.store(dispatcher_now_ns(), std::memory_order_relaxed);

            stats_.total_batches.fetch_add(1, std::memory_order_relaxed);
            stats_.total_events.fetch_add(dispatched, std::memory_order_relaxed);
        }
    }

    void drain_remaining() {
        const auto deadline =
            std::chrono::steady_clock::now() + config_.stop_drain_timeout;

        while (std::chrono::steady_clock::now() < deadline) {
            const auto dispatched = bus_.dispatch_once(config_.batch_size);

            if (dispatched == 0) {
                break;
            }

            stats_.total_batches.fetch_add(1, std::memory_order_relaxed);
            stats_.total_events.fetch_add(dispatched, std::memory_order_relaxed);
        }
    }

private:
    Bus& bus_;
    DispatcherConfig config_;

    AffinityHook affinity_hook_;
    ErrorHook error_hook_;

    std::atomic<DispatcherState> state_{DispatcherState::Created};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> pause_requested_{false};

    std::mutex lifecycle_mutex_;
    std::mutex cv_mutex_;
    std::condition_variable cv_;

    std::vector<std::thread> workers_;
    std::vector<DispatcherWorkerStats> worker_stats_;

    DispatcherStats stats_{};
};

} // namespace hft::exchange