#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// latency.hpp  —  3-component latency model + timed event queue
//
// Three components (all in nanoseconds):
//   1. Deterministic base (cabling + NIC + kernel minimum)
//   2. Log-normal jitter  (CPU scheduling, buffer allocation, GC)
//   3. Pareto burst tail  (exchange load spikes, packet re-tx)
//      Triggered with probability p_burst; sampled from Pareto(alpha)
//
// TimedEventQueue: min-heap for scheduling delayed events.
// ─────────────────────────────────────────────────────────────────────────────

#include "types.hpp"
#include <cstdint>
#include <cmath>
#include <random>
#include <vector>
#include <queue>
#include <functional>
#include <memory>
#include <algorithm>
#include <variant>
#include <string>
#include <unordered_map>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// LatencyProfile
// ─────────────────────────────────────────────────────────────────────────────
struct LatencyProfile {
    // Feed (market data) path — nanoseconds
    int64_t feed_base          = 500'000;      // 500 µs
    double  feed_jitter_mu     = 10.0;
    double  feed_jitter_sigma  = 1.5;
    double  feed_p_burst       = 0.002;
    double  feed_burst_alpha   = 1.5;
    int64_t feed_burst_scale   = 2'000'000;    // 2 ms

    // Order-entry path
    int64_t order_base         = 2'000'000;    // 2 ms
    double  order_jitter_mu    = 12.0;
    double  order_jitter_sigma = 1.8;
    double  order_p_burst      = 0.005;
    double  order_burst_alpha  = 1.3;
    int64_t order_burst_scale  = 5'000'000;    // 5 ms

    // Cancel path
    int64_t cancel_base        = 1'800'000;

    // Exchange ack processing
    int64_t ack_latency        = 100'000;      // 100 µs

    uint64_t seed              = 42;
};

// ─────────────────────────────────────────────────────────────────────────────
// LatencyModel  —  sampling engine for one LatencyProfile
// ─────────────────────────────────────────────────────────────────────────────
class LatencyModel {
public:
    explicit LatencyModel(const LatencyProfile& cfg, uint64_t seed_override = 0)
        : cfg_(cfg)
        , rng_(seed_override ? seed_override : cfg.seed)
        , lognorm_dist_(0.0, 1.0)
        , uniform_dist_(0.0, 1.0)
    {}

    int64_t feed_delay() {
        int64_t v = sample(cfg_.feed_base,
                           cfg_.feed_jitter_mu,  cfg_.feed_jitter_sigma,
                           cfg_.feed_p_burst,    cfg_.feed_burst_alpha,
                           cfg_.feed_burst_scale);
        feed_samples_.push_back(v);
        return v;
    }

    int64_t order_rtt() {
        int64_t v = sample(cfg_.order_base,
                           cfg_.order_jitter_mu,  cfg_.order_jitter_sigma,
                           cfg_.order_p_burst,    cfg_.order_burst_alpha,
                           cfg_.order_burst_scale);
        order_samples_.push_back(v);
        return v;
    }

    int64_t cancel_rtt() {
        return sample(cfg_.cancel_base,
                      cfg_.order_jitter_mu, cfg_.order_jitter_sigma,
                      cfg_.order_p_burst,   cfg_.order_burst_alpha,
                      cfg_.order_burst_scale);
    }

    // Latency percentiles (µs)
    struct Percentiles {
        double p50, p95, p99, p999, mean;
        size_t n;
    };

    Percentiles feed_percentiles()  const { return compute_pct(feed_samples_);  }
    Percentiles order_percentiles() const { return compute_pct(order_samples_); }

    const LatencyProfile& profile() const noexcept { return cfg_; }

    // Raw sample vectors (for latency histograms in analytics)
    const std::vector<int64_t>& raw_feed_samples()  const { return feed_samples_; }
    const std::vector<int64_t>& raw_order_samples() const { return order_samples_; }

private:
    LatencyProfile             cfg_;
    std::mt19937_64            rng_;
    std::normal_distribution<> lognorm_dist_;
    std::uniform_real_distribution<> uniform_dist_;

    std::vector<int64_t> feed_samples_;
    std::vector<int64_t> order_samples_;

    int64_t sample(int64_t base,
                   double mu, double sigma,
                   double p_burst, double burst_alpha, int64_t burst_scale)
    {
        // Log-normal jitter
        double z      = lognorm_dist_(rng_) * sigma + mu;
        int64_t jitter = static_cast<int64_t>(std::max(0.0, std::exp(z)));

        // Pareto burst
        int64_t burst = 0;
        if (p_burst > 0.0 && uniform_dist_(rng_) < p_burst) {
            double u  = std::max(1e-12, uniform_dist_(rng_));
            burst     = static_cast<int64_t>(burst_scale * std::pow(1.0 - u, -1.0 / burst_alpha));
        }
        return base + jitter + burst;
    }

    static Percentiles compute_pct(std::vector<int64_t> samples) {
        if (samples.empty()) return {0,0,0,0,0,0};
        std::sort(samples.begin(), samples.end());
        size_t n = samples.size();
        auto at  = [&](double pct) -> double {
            size_t idx = static_cast<size_t>(pct / 100.0 * n);
            if (idx >= n) idx = n - 1;
            return samples[idx] / 1000.0;  // → µs
        };
        double sum = 0.0;
        for (auto v : samples) sum += v;
        return { at(50), at(95), at(99), at(99.9), sum / n / 1000.0, n };
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Exchange presets
// ─────────────────────────────────────────────────────────────────────────────

inline LatencyProfile binance_colocation() {
    return LatencyProfile{
        .feed_base=40'000,  .feed_jitter_mu=9.2,  .feed_jitter_sigma=1.1,
        .feed_p_burst=0.001,.feed_burst_alpha=1.8, .feed_burst_scale=500'000,
        .order_base=400'000,.order_jitter_mu=11.2,.order_jitter_sigma=1.4,
        .order_p_burst=0.003,.order_burst_alpha=1.5,.order_burst_scale=1'000'000,
        .cancel_base=350'000,.ack_latency=60'000
    };
}

inline LatencyProfile binance_retail() {
    return LatencyProfile{
        .feed_base=18'000'000,.feed_jitter_mu=14.2,.feed_jitter_sigma=2.0,
        .feed_p_burst=0.01,  .feed_burst_alpha=1.2,.feed_burst_scale=20'000'000,
        .order_base=38'000'000,.order_jitter_mu=15.2,.order_jitter_sigma=2.2,
        .order_p_burst=0.02, .order_burst_alpha=1.1,.order_burst_scale=40'000'000,
        .cancel_base=35'000'000,.ack_latency=200'000
    };
}

inline LatencyProfile bybit_colocation() {
    return LatencyProfile{
        .feed_base=60'000,  .feed_jitter_mu=9.5, .feed_jitter_sigma=1.2,
        .feed_p_burst=0.0015,.feed_burst_alpha=1.7,.feed_burst_scale=600'000,
        .order_base=600'000,.order_jitter_mu=11.5,.order_jitter_sigma=1.5,
        .order_p_burst=0.004,.order_burst_alpha=1.4,.order_burst_scale=1'500'000,
        .cancel_base=500'000,.ack_latency=80'000
    };
}

inline LatencyProfile bybit_retail() {
    return LatencyProfile{
        .feed_base=22'000'000,.feed_jitter_mu=14.5,.feed_jitter_sigma=2.1,
        .feed_p_burst=0.012, .feed_burst_alpha=1.2,.feed_burst_scale=25'000'000,
        .order_base=45'000'000,.order_jitter_mu=15.5,.order_jitter_sigma=2.3,
        .order_p_burst=0.025,.order_burst_alpha=1.1,.order_burst_scale=50'000'000,
        .cancel_base=42'000'000,.ack_latency=250'000
    };
}

inline LatencyProfile okx_colocation() {
    return LatencyProfile{
        .feed_base=55'000,  .feed_jitter_mu=9.4, .feed_jitter_sigma=1.2,
        .feed_p_burst=0.002,.feed_burst_alpha=1.6,.feed_burst_scale=700'000,
        .order_base=550'000,.order_jitter_mu=11.4,.order_jitter_sigma=1.5,
        .order_p_burst=0.004,.order_burst_alpha=1.4,.order_burst_scale=1'200'000,
        .cancel_base=480'000,.ack_latency=75'000
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// TimedEventQueue  —  min-heap priority queue for delayed events
// ─────────────────────────────────────────────────────────────────────────────

// Forward-declared event types
struct BookUpdateEvt { L2Update update; };
struct BookUpdateL3Evt { L3Update update; };
struct TradeEvt     { Trade trade; };
struct OrderAckEvt  { Order order; bool accepted; };
struct FillDelivEvt { FillEvent fill; };

using TimedPayload = std::variant<
    BookUpdateEvt,
    BookUpdateL3Evt,
    TradeEvt,
    OrderAckEvt,
    FillDelivEvt
>;

struct TimedEvent {
    int64_t      deliver_at_ns = 0;
    int64_t      seq           = 0;   // tie-breaker (insertion order)
    TimedPayload payload;

    bool operator>(const TimedEvent& o) const noexcept {
        if (deliver_at_ns != o.deliver_at_ns) return deliver_at_ns > o.deliver_at_ns;
        return seq > o.seq;
    }
};

class TimedEventQueue {
public:
    void push(int64_t deliver_at_ns, TimedPayload payload) {
        heap_.push(TimedEvent{deliver_at_ns, ++counter_, std::move(payload)});
    }

    // Pop all events whose delivery time ≤ current_ns
    std::vector<TimedEvent> pop_ready(int64_t current_ns) {
        std::vector<TimedEvent> ready;
        while (!heap_.empty() && heap_.top().deliver_at_ns <= current_ns) {
            ready.push_back(std::move(const_cast<TimedEvent&>(heap_.top())));
            heap_.pop();
        }
        return ready;
    }

    std::optional<int64_t> peek_next_ts() const {
        if (heap_.empty()) return std::nullopt;
        return heap_.top().deliver_at_ns;
    }

    size_t size() const noexcept { return heap_.size(); }
    bool   empty() const noexcept { return heap_.empty(); }

private:
    using MinHeap = std::priority_queue<TimedEvent,
                                        std::vector<TimedEvent>,
                                        std::greater<TimedEvent>>;
    MinHeap heap_;
    int64_t counter_ = 0;
};

} // namespace hft