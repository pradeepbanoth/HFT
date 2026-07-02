#include "exchange_bus.hpp"
#include "exchange_dispatcher.hpp"
#include "exchange_bus_bridge.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace hft::exchange;

#define ASSERT_TRUE(cond, msg)                                      \
    do {                                                            \
        if (!(cond)) {                                              \
            std::cerr << "FAIL: " << msg << "\n";                  \
            std::exit(1);                                           \
        } else {                                                    \
            std::cout << "PASS: " << msg << "\n";                  \
        }                                                           \
    } while (0)

void test_exchange_bus_stress_100k() {
    ExchangeBus bus;
    bus.start();

    std::atomic<int> received{0};

    bus.subscribe<L1Update>(
        ExchangeEventType::L1Update,
        [&](const ExchangeEventHeader&, const L1Update&) {
            received.fetch_add(1, std::memory_order_relaxed);
        },
        "stress-sub"
    );

    DispatcherConfig cfg;
    cfg.worker_count = 4;
    cfg.batch_size = 512;
    cfg.idle_sleep = std::chrono::microseconds(10);

    ExchangeDispatcher<ExchangeBus> dispatcher(bus, cfg);
    ASSERT_TRUE(dispatcher.start(), "Stress dispatcher starts");

    constexpr int total_events = 100000;

    int published = 0;

    while (published < total_events) {
    if (bus.publish(
            ExchangeEventType::L1Update,
            L1Update{"BTC-USDT", 100.0, 101.0, 1.0, 1.0},
            EventPriority::Normal
        )) {
        ++published;
    } else {
        std::this_thread::yield();
      }
   }

    ASSERT_TRUE(published == total_events, "Stress published all 100k events");

    for (int i = 0; i < 200 && received.load() < total_events; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    dispatcher.stop();
    bus.stop();

    ASSERT_TRUE(received.load() == total_events, "Stress received all 100k events");

    auto snap = bus.metrics_snapshot();
    ASSERT_TRUE(snap.published == total_events, "Stress published metric");
    ASSERT_TRUE(snap.dispatched == total_events, "Stress dispatched metric");
}

void test_exchange_bus_multi_producer() {
    ExchangeBus bus;
    bus.start();

    std::atomic<int> received{0};

    bus.subscribe<TradeEvent>(
        ExchangeEventType::Trade,
        [&](const ExchangeEventHeader&, const TradeEvent&) {
            received.fetch_add(1, std::memory_order_relaxed);
        },
        "multi-producer-sub"
    );

    ExchangeDispatcher<ExchangeBus> dispatcher(
        bus,
        DispatcherConfig{4, 512, std::chrono::microseconds(10)}
    );

    dispatcher.start();

    constexpr int producer_count = 4;
    constexpr int events_per_producer = 25000;
    constexpr int total_events = producer_count * events_per_producer;

    std::vector<std::thread> producers;

    for (int p = 0; p < producer_count; ++p) {
        producers.emplace_back([&bus] {
           int published = 0;

           while (published < events_per_producer) {
        if (bus.publish(
                ExchangeEventType::Trade,
                TradeEvent{"ETH-USDT", 200.0, 1.0, true},
                EventPriority::High
            )) {
            ++published;
            } else {
              std::this_thread::yield();
            }
         }
      });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    for (int i = 0; i < 200 && received.load() < total_events; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    dispatcher.stop();
    bus.stop();

    ASSERT_TRUE(received.load() == total_events, "Multi-producer received all events");
}

void test_exchange_bus_priority_ordering() {
    ExchangeBus bus;
    bus.start();

    std::vector<int> order;

    bus.subscribe<L1Update>(
        ExchangeEventType::L1Update,
        [&](const ExchangeEventHeader& header, const L1Update&) {
            order.push_back(static_cast<int>(header.priority));
        },
        "priority-sub"
    );

    bus.publish(
        ExchangeEventType::L1Update,
        L1Update{"LOW", 1, 2, 1, 1},
        EventPriority::Low
    );

    bus.publish(
        ExchangeEventType::L1Update,
        L1Update{"HIGH", 1, 2, 1, 1},
        EventPriority::High
    );

    bus.publish(
        ExchangeEventType::L1Update,
        L1Update{"NORMAL", 1, 2, 1, 1},
        EventPriority::Normal
    );

    bus.dispatch_once(3);

    ASSERT_TRUE(order.size() == 3, "Priority test dispatched three events");
    ASSERT_TRUE(order[0] == static_cast<int>(EventPriority::High), "High priority first");
    ASSERT_TRUE(order[1] == static_cast<int>(EventPriority::Normal), "Normal priority second");
    ASSERT_TRUE(order[2] == static_cast<int>(EventPriority::Low), "Low priority third");

    bus.stop();
}

void test_exchange_bus_lifecycle_safety() {
    ExchangeBus bus;

    ASSERT_TRUE(!bus.publish(
        ExchangeEventType::L1Update,
        L1Update{"BTC-USDT", 1, 2, 1, 1}
    ), "Publish rejected before start");

    bus.start();

    ASSERT_TRUE(bus.publish(
        ExchangeEventType::L1Update,
        L1Update{"BTC-USDT", 1, 2, 1, 1}
    ), "Publish accepted after start");

    bus.stop();

    ASSERT_TRUE(!bus.publish(
        ExchangeEventType::L1Update,
        L1Update{"BTC-USDT", 1, 2, 1, 1}
    ), "Publish rejected after stop");
}

void test_exchange_bus_throughput_benchmark() {
    ExchangeBus bus;
    bus.start();

    std::atomic<int> received{0};

    bus.subscribe<L2Update>(
        ExchangeEventType::L2Update,
        [&](const ExchangeEventHeader&, const L2Update&) {
            received.fetch_add(1, std::memory_order_relaxed);
        },
        "throughput-sub"
    );

    ExchangeDispatcher<ExchangeBus> dispatcher(
        bus,
        DispatcherConfig{4, 1024, std::chrono::microseconds(5)}
    );

    dispatcher.start();

    constexpr int total_events = 250000;

    auto start = std::chrono::steady_clock::now();

    int published = 0;

      while (published < total_events) {
        if (bus.publish(
            ExchangeEventType::L2Update,
            L2Update{"BTC-USDT", 100.0 + published, 1.0, true, 1},
            EventPriority::Normal
        )) {
        ++published;
    } else {
        std::this_thread::yield();
    }
       }

     ASSERT_TRUE(published == total_events, "Throughput published all events");

    for (int i = 0; i < 300 && received.load() < total_events; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto end = std::chrono::steady_clock::now();

    dispatcher.stop();
    bus.stop();

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    const double events_per_sec =
        elapsed_ms > 0 ? (total_events * 1000.0) / elapsed_ms : 0.0;

    std::cout << "Throughput: " << events_per_sec << " events/sec\n";

    ASSERT_TRUE(received.load() == total_events, "Throughput received all events");
    ASSERT_TRUE(events_per_sec > 10000.0, "Throughput above baseline");
}

void test_advanced_exchange_bus_bridge() {
    using namespace hft::exchange;

    ExchangeBus bus;
    bus.start();

    ExchangeBusBridgeConfig config;
    config.venue_id = 7;
    config.backpressure_policy = BridgeBackpressurePolicy::YieldRetry;
    config.max_publish_retries = 1024;
    config.normalize_symbols = true;

    ExchangeBusBridge bridge(bus, config);

    ASSERT_TRUE(bridge.start(), "Advanced bridge starts");
    ASSERT_TRUE(bridge.running(), "Advanced bridge running");

    bool l2_received = false;
    bool ack_received = false;
    bool error_received = false;

    uint64_t l2_sequence = 0;
    uint32_t l2_venue = 0;

    bus.subscribe<L2Update>(
        ExchangeEventType::L2Update,
        [&](const ExchangeEventHeader& header, const L2Update& event) {
            l2_received = true;
            l2_sequence = header.sequence;
            l2_venue = header.venue_id;

            ASSERT_TRUE(event.symbol == "BTC-USDT", "Advanced bridge normalizes symbol");
            ASSERT_TRUE(header.venue_id == 7, "Advanced bridge venue id");
            ASSERT_TRUE(header.sequence == 1, "Advanced bridge L2 sequence");
        },
        "advanced-bridge-l2"
    );

    bus.subscribe<OrderAck>(
        ExchangeEventType::OrderAck,
        [&](const ExchangeEventHeader& header, const OrderAck& event) {
            ack_received = true;

            ASSERT_TRUE(event.client_order_id == "cid-100", "Advanced bridge ack client id");
            ASSERT_TRUE(event.venue_order_id == "vid-100", "Advanced bridge ack venue id");
            ASSERT_TRUE(event.symbol == "ETH-USDT", "Advanced bridge ack symbol normalized");
            ASSERT_TRUE(header.venue_id == 7, "Advanced bridge ack venue id metadata");
            ASSERT_TRUE(header.sequence == 2, "Advanced bridge ack sequence");
        },
        "advanced-bridge-ack"
    );

    bus.subscribe<VenueError>(
        ExchangeEventType::VenueError,
        [&](const ExchangeEventHeader& header, const VenueError& event) {
            error_received = true;

            ASSERT_TRUE(event.code == 429, "Advanced bridge error code");
            ASSERT_TRUE(event.message == "rate limit", "Advanced bridge error message");
            ASSERT_TRUE(header.venue_id == 7, "Advanced bridge error venue id metadata");
            ASSERT_TRUE(header.sequence == 3, "Advanced bridge error sequence");
        },
        "advanced-bridge-error"
    );

    ASSERT_TRUE(
        bridge.publish_l2("BTC/USDT", 100.5, 4.0, true, 1),
        "Advanced bridge publishes normalized L2"
    );

    ASSERT_TRUE(
        bridge.publish_order_ack("cid-100", "vid-100", "ETH/USDT"),
        "Advanced bridge publishes normalized ack"
    );

    ASSERT_TRUE(
        bridge.publish_venue_error(429, "rate limit"),
        "Advanced bridge publishes venue error"
    );

    const auto dispatched = bus.dispatch_once(16);

    ASSERT_TRUE(dispatched == 3, "Advanced bridge dispatched three events");
    ASSERT_TRUE(l2_received, "Advanced bridge L2 received");
    ASSERT_TRUE(ack_received, "Advanced bridge ack received");
    ASSERT_TRUE(error_received, "Advanced bridge error received");

    ASSERT_TRUE(l2_sequence == 1, "Advanced bridge captured L2 sequence");
    ASSERT_TRUE(l2_venue == 7, "Advanced bridge captured venue id");

    ASSERT_TRUE(
        bridge.stats().market_events_published.load() == 1,
        "Advanced bridge market metric"
    );

    ASSERT_TRUE(
        bridge.stats().execution_events_published.load() == 1,
        "Advanced bridge execution metric"
    );

    ASSERT_TRUE(
        bridge.stats().errors_published.load() == 1,
        "Advanced bridge error metric"
    );

    ASSERT_TRUE(
        bridge.stats().sequence_generated.load() == 3,
        "Advanced bridge sequence metric"
    );

    bridge.stop();

    ASSERT_TRUE(bridge.state() == BridgeState::Stopped, "Advanced bridge stops");

    ASSERT_TRUE(
        !bridge.publish_trade("BTC/USDT", 100.0, 1.0, true),
        "Advanced bridge rejects publish after stop"
    );

    bus.stop();
}


int main() {
    test_exchange_bus_lifecycle_safety();
    test_exchange_bus_priority_ordering();
    test_exchange_bus_stress_100k();
    test_exchange_bus_multi_producer();
    test_exchange_bus_throughput_benchmark();
    test_advanced_exchange_bus_bridge();

    std::cout << "\nAll production exchange bus tests passed.\n";
    return 0;
}