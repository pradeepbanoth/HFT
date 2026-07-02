#pragma once

#include "exchange_bus.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace hft::exchange {

enum class BridgeState : uint8_t {
    Created,
    Running,
    Stopping,
    Stopped
};

enum class BridgeBackpressurePolicy : uint8_t {
    Drop,
    YieldRetry,
    SpinRetry
};

struct ExchangeBusBridgeConfig {
    uint32_t venue_id{0};

    BridgeBackpressurePolicy backpressure_policy{
        BridgeBackpressurePolicy::YieldRetry
    };

    uint32_t max_publish_retries{256};

    bool normalize_symbols{true};
};

struct ExchangeBusBridgeStats {
    std::atomic<uint64_t> market_events_published{0};
    std::atomic<uint64_t> execution_events_published{0};
    std::atomic<uint64_t> errors_published{0};
    std::atomic<uint64_t> rejected_publishes{0};
    std::atomic<uint64_t> retry_attempts{0};
    std::atomic<uint64_t> sequence_generated{0};
};

class ExchangeBusBridge {
public:
    ExchangeBusBridge(ExchangeBus& bus, ExchangeBusBridgeConfig config = {})
        : bus_(bus), config_(config) {}

    ExchangeBusBridge(const ExchangeBusBridge&) = delete;
    ExchangeBusBridge& operator=(const ExchangeBusBridge&) = delete;

    bool start() noexcept {
        BridgeState expected = BridgeState::Created;
        if (state_.compare_exchange_strong(expected, BridgeState::Running)) {
            return true;
        }

        expected = BridgeState::Stopped;
        return state_.compare_exchange_strong(expected, BridgeState::Running);
    }

    void stop() noexcept {
        BridgeState expected = BridgeState::Running;
        if (state_.compare_exchange_strong(expected, BridgeState::Stopping)) {
            state_.store(BridgeState::Stopped, std::memory_order_release);
        }
    }

    bool running() const noexcept {
        return state_.load(std::memory_order_acquire) == BridgeState::Running;
    }

    BridgeState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    bool publish_l1(
        std::string symbol,
        double bid,
        double ask,
        double bid_qty,
        double ask_qty,
        EventPriority priority = EventPriority::High
    ) {
        symbol = normalize(std::move(symbol));

        return publish_market(
            ExchangeEventType::L1Update,
            L1Update{std::move(symbol), bid, ask, bid_qty, ask_qty},
            priority
        );
    }

    bool publish_l2(
        std::string symbol,
        double price,
        double qty,
        bool is_bid,
        uint32_t level,
        EventPriority priority = EventPriority::High
    ) {
        symbol = normalize(std::move(symbol));

        return publish_market(
            ExchangeEventType::L2Update,
            L2Update{std::move(symbol), price, qty, is_bid, level},
            priority
        );
    }

    bool publish_trade(
        std::string symbol,
        double price,
        double qty,
        bool aggressor_buy,
        EventPriority priority = EventPriority::High
    ) {
        symbol = normalize(std::move(symbol));

        return publish_market(
            ExchangeEventType::Trade,
            TradeEvent{std::move(symbol), price, qty, aggressor_buy},
            priority
        );
    }

    bool publish_order_ack(
        std::string client_order_id,
        std::string venue_order_id,
        std::string symbol,
        EventPriority priority = EventPriority::High
    ) {
        symbol = normalize(std::move(symbol));

        return publish_execution(
            ExchangeEventType::OrderAck,
            OrderAck{
                std::move(client_order_id),
                std::move(venue_order_id),
                std::move(symbol)
            },
            priority
        );
    }

   bool publish_order_reject(
    std::string client_order_id,
    std::string reason,
    EventPriority priority = EventPriority::High
    ) {
    OrderReject reject{};
    reject.client_order_id = std::move(client_order_id);
    reject.reason = std::move(reason);

    return publish_execution(
        ExchangeEventType::OrderReject,
        std::move(reject),
        priority
      );
   }

    bool publish_fill(
        std::string client_order_id,
        std::string venue_order_id,
        std::string symbol,
        double price,
        double qty,
        double fee,
        EventPriority priority = EventPriority::High
    ) {
        symbol = normalize(std::move(symbol));

        return publish_execution(
            ExchangeEventType::Fill,
            FillEvent{
                std::move(client_order_id),
                std::move(venue_order_id),
                std::move(symbol),
                price,
                qty,
                fee
            },
            priority
        );
    }

    bool publish_venue_error(
    int code,
    std::string message,
    EventPriority priority = EventPriority::High
    ) {
    VenueError error{};
    error.code = code;
    error.message = std::move(message);

    const bool ok = publish_with_backpressure(
        ExchangeEventType::VenueError,
        std::move(error),
        priority
    );

    if (ok) {
        stats_.errors_published.fetch_add(1, std::memory_order_relaxed);
    } else {
        stats_.rejected_publishes.fetch_add(1, std::memory_order_relaxed);
    }

    return ok;
    }

    const ExchangeBusBridgeStats& stats() const noexcept {
        return stats_;
    }

private:
    template <typename Payload>
    bool publish_market(
        ExchangeEventType type,
        Payload payload,
        EventPriority priority
    ) {
        const bool ok = publish_with_backpressure(type, std::move(payload), priority);

        if (ok) {
            stats_.market_events_published.fetch_add(1, std::memory_order_relaxed);
        } else {
            stats_.rejected_publishes.fetch_add(1, std::memory_order_relaxed);
        }

        return ok;
    }

    template <typename Payload>
    bool publish_execution(
        ExchangeEventType type,
        Payload payload,
        EventPriority priority
    ) {
        const bool ok = publish_with_backpressure(type, std::move(payload), priority);

        if (ok) {
            stats_.execution_events_published.fetch_add(1, std::memory_order_relaxed);
        } else {
            stats_.rejected_publishes.fetch_add(1, std::memory_order_relaxed);
        }

        return ok;
    }

    template <typename Payload>
    bool publish_with_backpressure(
        ExchangeEventType type,
        Payload payload,
        EventPriority priority
    ) {
        if (!running()) {
            return false;
        }

        const auto sequence =
            next_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;

        stats_.sequence_generated.store(sequence, std::memory_order_relaxed);

        for (uint32_t attempt = 0; attempt <= config_.max_publish_retries; ++attempt) {
            if (bus_.publish(
                    type,
                    payload,
                    priority,
                    config_.venue_id,
                    0,
                    sequence
                )) {
                return true;
            }

            stats_.retry_attempts.fetch_add(1, std::memory_order_relaxed);

            switch (config_.backpressure_policy)
{
            case BridgeBackpressurePolicy::Drop:
    return false;

            case BridgeBackpressurePolicy::YieldRetry:
    std::this_thread::yield();
    break;

            case BridgeBackpressurePolicy::SpinRetry:
    break;
}

            if (config_.backpressure_policy ==
    BridgeBackpressurePolicy::YieldRetry)
       {
    std::this_thread::yield();
        } {
                std::this_thread::yield();
            }
        }

        return false;
    }

    std::string normalize(std::string symbol) const {
        if (!config_.normalize_symbols) {
            return symbol;
        }

        for (char& c : symbol) {
            if (c == '/') c = '-';
        }

        return symbol;
    }

private:
    ExchangeBus& bus_;
    ExchangeBusBridgeConfig config_;

    std::atomic<BridgeState> state_{BridgeState::Created};
    std::atomic<uint64_t> next_sequence_{0};

    ExchangeBusBridgeStats stats_{};
};

} // namespace hft::exchange