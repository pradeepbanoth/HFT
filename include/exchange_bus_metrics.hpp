#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace hft::exchange {

constexpr std::size_t LATENCY_BUCKETS = 16;

struct LatencyHistogramSnapshot {
    uint64_t count{0};
    uint64_t total_ns{0};
    uint64_t min_ns{0};
    uint64_t max_ns{0};
    uint64_t avg_ns{0};

    uint64_t p50_ns{0};
    uint64_t p90_ns{0};
    uint64_t p99_ns{0};

    std::array<uint64_t, LATENCY_BUCKETS> buckets{};
};

class AtomicLatencyHistogram {
public:
    void observe(uint64_t ns) noexcept {
        count_.fetch_add(1, std::memory_order_relaxed);
        total_ns_.fetch_add(ns, std::memory_order_relaxed);

        update_min(ns);
        update_max(ns);

        buckets_[bucket_index(ns)].fetch_add(1, std::memory_order_relaxed);
    }

    LatencyHistogramSnapshot snapshot() const noexcept {
        LatencyHistogramSnapshot s;

        s.count = count_.load(std::memory_order_relaxed);
        s.total_ns = total_ns_.load(std::memory_order_relaxed);
        s.min_ns = s.count == 0 ? 0 : min_ns_.load(std::memory_order_relaxed);
        s.max_ns = s.count == 0 ? 0 : max_ns_.load(std::memory_order_relaxed);
        s.avg_ns = s.count == 0 ? 0 : s.total_ns / s.count;

        for (std::size_t i = 0; i < LATENCY_BUCKETS; ++i) {
            s.buckets[i] = buckets_[i].load(std::memory_order_relaxed);
        }

        s.p50_ns = percentile_estimate(s, 50);
        s.p90_ns = percentile_estimate(s, 90);
        s.p99_ns = percentile_estimate(s, 99);

        return s;
    }

private:
    static std::size_t bucket_index(uint64_t ns) noexcept {
        if (ns <= 100) return 0;
        if (ns <= 250) return 1;
        if (ns <= 500) return 2;
        if (ns <= 1'000) return 3;
        if (ns <= 2'500) return 4;
        if (ns <= 5'000) return 5;
        if (ns <= 10'000) return 6;
        if (ns <= 25'000) return 7;
        if (ns <= 50'000) return 8;
        if (ns <= 100'000) return 9;
        if (ns <= 250'000) return 10;
        if (ns <= 500'000) return 11;
        if (ns <= 1'000'000) return 12;
        if (ns <= 5'000'000) return 13;
        if (ns <= 10'000'000) return 14;
        return 15;
    }

    static uint64_t bucket_upper_bound(std::size_t idx) noexcept {
        static constexpr std::array<uint64_t, LATENCY_BUCKETS> bounds{
            100,
            250,
            500,
            1'000,
            2'500,
            5'000,
            10'000,
            25'000,
            50'000,
            100'000,
            250'000,
            500'000,
            1'000'000,
            5'000'000,
            10'000'000,
            50'000'000
        };

        return bounds[idx];
    }

    static uint64_t percentile_estimate(
        const LatencyHistogramSnapshot& s,
        uint64_t percentile
    ) noexcept {
        if (s.count == 0) return 0;

        const uint64_t target = (s.count * percentile + 99) / 100;
        uint64_t running = 0;

        for (std::size_t i = 0; i < LATENCY_BUCKETS; ++i) {
            running += s.buckets[i];

            if (running >= target) {
                return bucket_upper_bound(i);
            }
        }

        return s.max_ns;
    }

    void update_min(uint64_t value) noexcept {
        auto current = min_ns_.load(std::memory_order_relaxed);

        while ((current == 0 || value < current) &&
               !min_ns_.compare_exchange_weak(
                   current,
                   value,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed
               )) {}
    }

    void update_max(uint64_t value) noexcept {
        auto current = max_ns_.load(std::memory_order_relaxed);

        while (value > current &&
               !max_ns_.compare_exchange_weak(
                   current,
                   value,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed
               )) {}
    }

private:
    std::atomic<uint64_t> count_{0};
    std::atomic<uint64_t> total_ns_{0};
    std::atomic<uint64_t> min_ns_{0};
    std::atomic<uint64_t> max_ns_{0};
    std::array<std::atomic<uint64_t>, LATENCY_BUCKETS> buckets_{};
};

struct ExchangeBusMetricsSnapshot {
    uint64_t published{0};
    uint64_t dispatched{0};
    uint64_t dropped{0};
    uint64_t rejected_not_running{0};

    uint64_t subscribers{0};

    uint64_t high_depth{0};
    uint64_t normal_depth{0};
    uint64_t low_depth{0};
    uint64_t total_depth{0};

    uint64_t slow_consumers{0};
    uint64_t max_queue_depth_seen{0};

    LatencyHistogramSnapshot publish_latency{};
    LatencyHistogramSnapshot dispatch_latency{};
    LatencyHistogramSnapshot consumer_latency{};
};

struct ExchangeBusMetrics {
    std::atomic<uint64_t> published{0};
    std::atomic<uint64_t> dispatched{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> rejected_not_running{0};

    std::atomic<uint64_t> subscribers{0};

    std::atomic<uint64_t> high_depth{0};
    std::atomic<uint64_t> normal_depth{0};
    std::atomic<uint64_t> low_depth{0};

    std::atomic<uint64_t> replayed{0};
    std::atomic<uint64_t> publish_latency_ns_total{0};
    std::atomic<uint64_t> dispatch_latency_ns_total{0};
    std::atomic<uint64_t> max_queue_latency_ns{0};

    std::atomic<uint64_t> slow_consumers{0};
    std::atomic<uint64_t> max_queue_depth_seen{0};

    AtomicLatencyHistogram publish_latency;
    AtomicLatencyHistogram dispatch_latency;
    AtomicLatencyHistogram consumer_latency;

    void observe_depths(uint64_t high, uint64_t normal, uint64_t low) noexcept {
        high_depth.store(high, std::memory_order_relaxed);
        normal_depth.store(normal, std::memory_order_relaxed);
        low_depth.store(low, std::memory_order_relaxed);

        const uint64_t total = high + normal + low;
        update_max_depth(total);
    }

    ExchangeBusMetricsSnapshot snapshot() const noexcept {
        ExchangeBusMetricsSnapshot s;

        s.published = published.load(std::memory_order_relaxed);
        s.dispatched = dispatched.load(std::memory_order_relaxed);
        s.dropped = dropped.load(std::memory_order_relaxed);
        s.rejected_not_running = rejected_not_running.load(std::memory_order_relaxed);

        s.subscribers = subscribers.load(std::memory_order_relaxed);

        s.high_depth = high_depth.load(std::memory_order_relaxed);
        s.normal_depth = normal_depth.load(std::memory_order_relaxed);
        s.low_depth = low_depth.load(std::memory_order_relaxed);
        s.total_depth = s.high_depth + s.normal_depth + s.low_depth;

        s.slow_consumers = slow_consumers.load(std::memory_order_relaxed);
        s.max_queue_depth_seen = max_queue_depth_seen.load(std::memory_order_relaxed);

        s.publish_latency = publish_latency.snapshot();
        s.dispatch_latency = dispatch_latency.snapshot();
        s.consumer_latency = consumer_latency.snapshot();

        return s;
    }

private:
    void update_max_depth(uint64_t value) noexcept {
        auto current = max_queue_depth_seen.load(std::memory_order_relaxed);

        while (value > current &&
               !max_queue_depth_seen.compare_exchange_weak(
                   current,
                   value,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed
               )) {}
    }
};



} // namespace hft::exchange