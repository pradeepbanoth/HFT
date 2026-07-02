#pragma once

#include "lock_free_ring_buffer.hpp"
#include "exchange_bus_metrics.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hft::exchange {

using EventId = uint64_t;
using SubscriptionId = uint64_t;
using TimestampNs = uint64_t;

inline TimestampNs now_ns() noexcept {
    return static_cast<TimestampNs>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

enum class ExchangeEventType : uint16_t {
    Unknown = 0,
    L1Update,
    L2Update,
    Trade,
    OrderAck,
    OrderReject,
    Fill,
    CancelAck,
    ReplaceAck,
    CancelReject,
    ReplaceReject,
    VenueError,
    Heartbeat,
    SessionState,
    RiskEvent,
    Custom
};

enum class EventPriority : uint8_t {
    High = 0,
    Normal = 1,
    Low = 2
};

enum class BusState : uint8_t {
    Created,
    Running,
    Stopping,
    Stopped
};

enum class BackpressurePolicy : uint8_t {
    DropNewest,
    DropOldest,
    Block
};

struct ExchangeEventHeader {
    EventId id{0};
    ExchangeEventType type{ExchangeEventType::Unknown};
    EventPriority priority{EventPriority::Normal};

    TimestampNs created_ns{0};
    TimestampNs published_ns{0};
    TimestampNs dispatched_ns{0};

    uint32_t venue_id{0};
    uint32_t symbol_id{0};
    uint64_t sequence{0};

    bool replayed{false};
};

template <typename Payload>
struct ExchangeEvent {
    ExchangeEventHeader header{};
    Payload payload{};
};

struct L1Update {
    std::string symbol;
    double bid{0.0};
    double ask{0.0};
    double bid_qty{0.0};
    double ask_qty{0.0};
};

struct L2Update {
    std::string symbol;
    double price{0.0};
    double qty{0.0};
    bool is_bid{false};
    uint32_t level{0};
};

struct TradeEvent {
    std::string symbol;
    double price{0.0};
    double qty{0.0};
    bool aggressor_buy{false};
};

struct OrderAck {
    std::string client_order_id;
    std::string venue_order_id;
    std::string symbol;
};

struct OrderReject {
    std::string client_order_id;
    std::string symbol;
    std::string reason;
};

struct FillEvent {
    std::string client_order_id;
    std::string venue_order_id;
    std::string symbol;
    double price{0.0};
    double qty{0.0};
    double fee{0.0};
};

struct CancelAck {
    std::string client_order_id;
    std::string venue_order_id;
    std::string symbol;
};

struct ReplaceAck {
    std::string old_client_order_id;
    std::string new_client_order_id;
    std::string venue_order_id;
    std::string symbol;
};

struct VenueError {
    int code{0};
    std::string venue;
    std::string message;
};

class IExchangeBusEvent {
public:
    virtual ~IExchangeBusEvent() = default;
    virtual const ExchangeEventHeader& header() const noexcept = 0;
    virtual ExchangeEventHeader& mutable_header() noexcept = 0;
};

template <typename Payload>
class TypedBusEvent final : public IExchangeBusEvent {
public:
    explicit TypedBusEvent(ExchangeEvent<Payload> event)
        : event_(std::move(event)) {}

    const ExchangeEventHeader& header() const noexcept override {
        return event_.header;
    }

    ExchangeEventHeader& mutable_header() noexcept override {
        return event_.header;
    }

    const Payload& payload() const noexcept {
        return event_.payload;
    }

private:
    ExchangeEvent<Payload> event_;
};

using RawEventPtr = std::shared_ptr<IExchangeBusEvent>;
using RawCallback = std::function<void(const IExchangeBusEvent&)>;
using RawFilter = std::function<bool(const IExchangeBusEvent&)>;
using RecorderHook = std::function<void(const IExchangeBusEvent&)>;
using ReplayHook = std::function<void(const IExchangeBusEvent&)>;

struct SubscriptionOptions {
    ExchangeEventType type{ExchangeEventType::Unknown};
    EventPriority min_priority{EventPriority::Low};
    std::string name;
    bool active{true};
};

struct Subscription {
    SubscriptionId id{0};
    SubscriptionOptions options{};
    RawCallback callback{};
    RawFilter filter{};
};

struct ExchangeBusConfig {
    size_t high_capacity{4096};
    size_t normal_capacity{4096};
    size_t low_capacity{4096};

    BackpressurePolicy backpressure{BackpressurePolicy::DropNewest};
    bool record_events{false};
};

using ExchangeBusStats = ExchangeBusMetrics;

class ExchangeBus {
public:
    static constexpr std::size_t ChannelCapacity = 4096;

    explicit ExchangeBus(ExchangeBusConfig config = {})
        : config_(config) {}

    ExchangeBus(const ExchangeBus&) = delete;
    ExchangeBus& operator=(const ExchangeBus&) = delete;

    ~ExchangeBus() {
        stop();
    }

    void start() {
        BusState expected = BusState::Created;

        if (state_.compare_exchange_strong(expected, BusState::Running)) {
            return;
        }

        expected = BusState::Stopped;
        state_.compare_exchange_strong(expected, BusState::Running);
    }

    void stop() {
        BusState expected = BusState::Running;

        if (state_.compare_exchange_strong(expected, BusState::Stopping)) {
            drain();
            state_.store(BusState::Stopped, std::memory_order_release);
        }
    }

    bool running() const noexcept {
        return state_.load(std::memory_order_acquire) == BusState::Running;
    }

    BusState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    template <typename Payload>
    bool publish(
        ExchangeEventType type,
        Payload payload,
        EventPriority priority = EventPriority::Normal,
        uint32_t venue_id = 0,
        uint32_t symbol_id = 0,
        uint64_t sequence = 0
    ) {
        if (!running()) {
            stats_.rejected_not_running.fetch_add(1, std::memory_order_relaxed);
            stats_.dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const auto start_ns = now_ns();

        ExchangeEvent<Payload> event{};
        event.header.id = next_event_id_.fetch_add(1, std::memory_order_relaxed);
        event.header.type = type;
        event.header.priority = priority;
        event.header.created_ns = start_ns;
        event.header.published_ns = start_ns;
        event.header.venue_id = venue_id;
        event.header.symbol_id = symbol_id;
        event.header.sequence = sequence;
        event.payload = std::move(payload);

        auto wrapped = std::make_shared<TypedBusEvent<Payload>>(std::move(event));

        if (config_.record_events && recorder_) {
            recorder_(*wrapped);
        }

        if (!push_event(priority, std::move(wrapped))) {
            stats_.dropped.fetch_add(1, std::memory_order_relaxed);
            refresh_depths();
            return false;
        }

        stats_.published.fetch_add(1, std::memory_order_relaxed);

        const auto end_ns = now_ns();
        stats_.publish_latency_ns_total.fetch_add(end_ns - start_ns, std::memory_order_relaxed);

        refresh_depths();
        return true;
    }

    ExchangeBusMetricsSnapshot metrics_snapshot() const noexcept {
    return stats_.snapshot();
     }

    void set_slow_consumer_threshold_ns(uint64_t threshold_ns) noexcept {
    slow_consumer_threshold_ns_ = threshold_ns;
     }

    template <typename Payload>
    bool replay(
        ExchangeEventType type,
        Payload payload,
        EventPriority priority = EventPriority::Normal,
        uint32_t venue_id = 0,
        uint32_t symbol_id = 0,
        uint64_t sequence = 0
    ) {
        if (!running()) return false;

        ExchangeEvent<Payload> event{};
        event.header.id = next_event_id_.fetch_add(1, std::memory_order_relaxed);
        event.header.type = type;
        event.header.priority = priority;
        event.header.created_ns = now_ns();
        event.header.published_ns = event.header.created_ns;
        event.header.venue_id = venue_id;
        event.header.symbol_id = symbol_id;
        event.header.sequence = sequence;
        event.header.replayed = true;
        event.payload = std::move(payload);

        auto wrapped = std::make_shared<TypedBusEvent<Payload>>(std::move(event));

        if (replay_hook_) {
            replay_hook_(*wrapped);
        }

        const bool pushed = push_event(priority, std::move(wrapped));

        if (pushed) {
            stats_.replayed.fetch_add(1, std::memory_order_relaxed);
        } else {
            stats_.dropped.fetch_add(1, std::memory_order_relaxed);
        }

        refresh_depths();
        return pushed;
    }

    template <typename Payload>
    SubscriptionId subscribe(
        ExchangeEventType type,
        std::function<void(const ExchangeEventHeader&, const Payload&)> callback,
        std::string name = {},
        EventPriority min_priority = EventPriority::Low
    ) {
        SubscriptionOptions options;
        options.type = type;
        options.name = std::move(name);
        options.min_priority = min_priority;

        RawCallback raw = [cb = std::move(callback)](const IExchangeBusEvent& base) {
            const auto* typed = dynamic_cast<const TypedBusEvent<Payload>*>(&base);
            if (!typed) return;
            cb(typed->header(), typed->payload());
        };

        return subscribe_raw(std::move(options), std::move(raw), {});
    }

    template <typename Payload>
    SubscriptionId subscribe_filtered(
        ExchangeEventType type,
        std::function<bool(const ExchangeEventHeader&, const Payload&)> filter,
        std::function<void(const ExchangeEventHeader&, const Payload&)> callback,
        std::string name = {},
        EventPriority min_priority = EventPriority::Low
    ) {
        SubscriptionOptions options;
        options.type = type;
        options.name = std::move(name);
        options.min_priority = min_priority;

        RawFilter raw_filter = [f = std::move(filter)](const IExchangeBusEvent& base) {
            const auto* typed = dynamic_cast<const TypedBusEvent<Payload>*>(&base);
            if (!typed) return false;
            return f(typed->header(), typed->payload());
        };

        RawCallback raw_callback = [cb = std::move(callback)](const IExchangeBusEvent& base) {
            const auto* typed = dynamic_cast<const TypedBusEvent<Payload>*>(&base);
            if (!typed) return;
            cb(typed->header(), typed->payload());
        };

        return subscribe_raw(std::move(options), std::move(raw_callback), std::move(raw_filter));
    }

    SubscriptionId subscribe_raw(
        SubscriptionOptions options,
        RawCallback callback,
        RawFilter filter = {}
    ) {
        if (!callback) return 0;

        Subscription sub;
        sub.id = next_subscription_id_.fetch_add(1, std::memory_order_relaxed);
        sub.options = std::move(options);
        sub.callback = std::move(callback);
        sub.filter = std::move(filter);

        const auto id = sub.id;

        {
            std::scoped_lock lock(sub_mutex_);
            subscriptions_[id] = std::move(sub);
            stats_.subscribers.store(subscriptions_.size(), std::memory_order_relaxed);
        }

        return id;
    }

    bool unsubscribe(SubscriptionId id) {
        std::scoped_lock lock(sub_mutex_);

        const auto erased = subscriptions_.erase(id);
        stats_.subscribers.store(subscriptions_.size(), std::memory_order_relaxed);

        return erased > 0;
    }

    size_t dispatch_once(size_t max_events = 1024) {
        size_t count = 0;

        while (count < max_events) {
            auto event = pop_next();
            if (!event) break;

            dispatch_event(*event);
            ++count;
        }

        return count;
    }

    size_t dispatch_batch(size_t max_events = 4096) {
        std::vector<RawEventPtr> batch;
        batch.reserve(max_events);

        drain_channel(high_, batch, max_events);

        if (batch.size() < max_events) {
            drain_channel(normal_, batch, max_events - batch.size());
        }

        if (batch.size() < max_events) {
            drain_channel(low_, batch, max_events - batch.size());
        }

        for (const auto& event : batch) {
            dispatch_event(event);
        }

        refresh_depths();
        return batch.size();
    }

    void drain() {
        while (dispatch_batch(4096) > 0) {}
    }

    void set_recorder_hook(RecorderHook hook) {
        std::scoped_lock lock(hook_mutex_);
        recorder_ = std::move(hook);
    }

    void set_replay_hook(ReplayHook hook) {
        std::scoped_lock lock(hook_mutex_);
        replay_hook_ = std::move(hook);
    }

    const ExchangeBusStats& stats() const noexcept {
        return stats_;
    }

private:
    LockFreeRingBuffer<RawEventPtr, ChannelCapacity>& channel_for(EventPriority priority) {
        switch (priority) {
            case EventPriority::High: return high_;
            case EventPriority::Normal: return normal_;
            case EventPriority::Low: return low_;
        }

        return normal_;
    }

    bool push_event(EventPriority priority, RawEventPtr event) {
        auto& channel = channel_for(priority);

        if (!channel.try_push(std::move(event))) {
            if (config_.backpressure == BackpressurePolicy::DropNewest) {
                return false;
            }

            if (config_.backpressure == BackpressurePolicy::DropOldest) {
                RawEventPtr dropped;
                channel.try_pop(dropped);
                stats_.dropped.fetch_add(1, std::memory_order_relaxed);
                return channel.try_push(std::move(event));
            }

            if (config_.backpressure == BackpressurePolicy::Block) {
                while (!channel.try_push(event)) {
                    RawEventPtr dropped;
                    if (channel.try_pop(dropped)) {
                        stats_.dropped.fetch_add(1, std::memory_order_relaxed);
                    }
                }

                return true;
            }
        }

        return true;
    }

    std::optional<RawEventPtr> pop_next() {
        RawEventPtr event;

        if (high_.try_pop(event)) {
            refresh_depths();
            return event;
        }

        if (normal_.try_pop(event)) {
            refresh_depths();
            return event;
        }

        if (low_.try_pop(event)) {
            refresh_depths();
            return event;
        }

        refresh_depths();
        return std::nullopt;
    }

    void drain_channel(
        LockFreeRingBuffer<RawEventPtr, ChannelCapacity>& channel,
        std::vector<RawEventPtr>& out,
        size_t max_items
    ) {
        RawEventPtr event;

        size_t drained = 0;

        while (drained < max_items && channel.try_pop(event)) {
            out.push_back(std::move(event));
            event.reset();
            ++drained;
        }
    }

    void dispatch_event(const RawEventPtr& event) {
        const auto dispatch_start = now_ns();

        event->mutable_header().dispatched_ns = dispatch_start;

        const auto queue_latency = dispatch_start - event->header().published_ns;
        update_max(stats_.max_queue_latency_ns, queue_latency);

        std::vector<Subscription> snapshot;

        {
            std::scoped_lock lock(sub_mutex_);
            snapshot.reserve(subscriptions_.size());

            for (const auto& [_, sub] : subscriptions_) {
                if (!sub.options.active) continue;
                if (sub.options.type != event->header().type) continue;

                if (static_cast<uint8_t>(event->header().priority) >
                    static_cast<uint8_t>(sub.options.min_priority)) {
                    continue;
                }

                if (sub.filter && !sub.filter(*event)) {
                    continue;
                }

                snapshot.push_back(sub);
            }
        }

        for (const auto& sub : snapshot) {
            sub.callback(*event);
            stats_.dispatched.fetch_add(1, std::memory_order_relaxed);
        }

        const auto dispatch_end = now_ns();
        stats_.dispatch_latency_ns_total.fetch_add(
            dispatch_end - dispatch_start,
            std::memory_order_relaxed
        );
    }

    static void update_max(std::atomic<uint64_t>& target, uint64_t value) {
        uint64_t current = target.load(std::memory_order_relaxed);

        while (value > current &&
               !target.compare_exchange_weak(
                   current,
                   value,
                   std::memory_order_relaxed
               )) {}
    }

    void refresh_depths() {
        stats_.high_depth.store(high_.size_approx(), std::memory_order_relaxed);
        stats_.normal_depth.store(normal_.size_approx(), std::memory_order_relaxed);
        stats_.low_depth.store(low_.size_approx(), std::memory_order_relaxed);
    }

private:
    ExchangeBusConfig config_{};

    std::atomic<BusState> state_{BusState::Created};
    std::atomic<EventId> next_event_id_{1};
    std::atomic<SubscriptionId> next_subscription_id_{1};

    LockFreeRingBuffer<RawEventPtr, ChannelCapacity> high_;
    LockFreeRingBuffer<RawEventPtr, ChannelCapacity> normal_;
    LockFreeRingBuffer<RawEventPtr, ChannelCapacity> low_;

    mutable std::mutex sub_mutex_;
    std::unordered_map<SubscriptionId, Subscription> subscriptions_;

    mutable std::mutex hook_mutex_;
    RecorderHook recorder_{};
    ReplayHook replay_hook_{};

    ExchangeBusStats stats_{};
    uint64_t slow_consumer_threshold_ns_{1'000'000};
};

} // namespace hft::exchange