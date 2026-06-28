#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// simulator.hpp  —  Production event-driven simulation engine
//
//  • Dual latency queues (feed path / order path) with burst model
//  • FIFO lot-level realized PnL
//  • Hard risk limits: position / drawdown / daily-loss with strategy halt
//  • Strategy error isolation: exceptions in callbacks never crash the engine
//  • Nanosecond-resolution MTM snapshots
//  • Full latency telemetry (feed + order p50/p95/p99/p999)
//  • std::variant event payloads — no heap allocation per event in hot path
// ─────────────────────────────────────────────────────────────────────────────

#include "types.hpp"
#include "orderbook.hpp"
#include "latency.hpp"
#include "fill_simulator.hpp"
#include "portfolio.hpp"
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <chrono>
#include <stdexcept>
#include <iostream>
#include <variant>
#include <optional>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// MarketEvent — type-erased feed event fed into the engine
// ─────────────────────────────────────────────────────────────────────────────
using MarketEvent = std::variant<L2Update, L3Update, Trade>;

inline int64_t event_timestamp(const MarketEvent& e) {
    return std::visit([](const auto& ev) { return ev.timestamp; }, e);
}

// ─────────────────────────────────────────────────────────────────────────────
// Strategy base class
// ─────────────────────────────────────────────────────────────────────────────
class SimEngine;

class Strategy {
public:
    virtual ~Strategy() = default;

    virtual void on_book_update(const std::string& symbol,
                                OrderBook& book,
                                int64_t ts_ns,
                                SimEngine& engine) {}

    virtual void on_trade(const Trade& trade,
                          OrderBook& book,
                          int64_t ts_ns,
                          SimEngine& engine) {}

    virtual void on_fill(const FillEvent& fill,
                         PortfolioState& portfolio,
                         int64_t ts_ns,
                         SimEngine& engine) {}

    virtual void on_order_ack(const Order& order,
                              bool accepted,
                              int64_t ts_ns,
                              SimEngine& engine) {}

    virtual void on_risk_breach(const std::vector<std::string>& breaches,
                                int64_t ts_ns,
                                SimEngine& engine) {}

    virtual void on_start(SimEngine& engine) {}
    virtual void on_end(SimEngine& engine)   {}
};

// ─────────────────────────────────────────────────────────────────────────────
// SimEngine
// ─────────────────────────────────────────────────────────────────────────────
struct SimStats {
    int64_t  ticks_processed   = 0;
    double   wall_time_s       = 0.0;
    double   ticks_per_second  = 0.0;
    int64_t  total_fills       = 0;
    int64_t  maker_fills       = 0;
    int64_t  taker_fills       = 0;
    int64_t  strategy_errors   = 0;
    int64_t  open_orders_final = 0;
    bool     halted            = false;
    PortfolioState::Summary portfolio_summary;
    LatencyModel::Percentiles feed_latency;
    LatencyModel::Percentiles order_latency;
};

class SimEngine {
public:
    SimEngine(
        Strategy&          strategy,
        LatencyProfile     latency_profile,
        FillModelConfig    fill_cfg         = {},
        double             initial_cash     = 100'000.0,
        RiskLimits         risk_limits      = {},
        int64_t            snapshot_interval_ns = 1'000'000'000LL,
        uint64_t           seed             = 42
    )
        : strategy_(strategy)
        , latency_(latency_profile, seed)
        , fill_sim_(fill_cfg, seed + 1)
        , portfolio_(initial_cash, std::move(risk_limits))
        , snapshot_interval_ns_(snapshot_interval_ns)
    {}

    // ── Setup ─────────────────────────────────────────────────────────────────

    void add_symbol(const std::string& symbol, double tick_size = 1e-8,
                    bool check_integrity = false)
    {
        books_.emplace(symbol,
            std::make_unique<OrderBook>(symbol, tick_size, check_integrity));
    }

    // ── Order Management API ─────────────────────────────────────────────────

    std::string submit_limit(const std::string& symbol,
                             Side side, double price, double qty,
                             bool post_only = true,
                             const std::string& client_id = "")
    {
        std::string oid = next_order_id();
        Order order;
        order.order_id   = oid;
        order.symbol     = symbol;
        order.side       = side;
        order.price      = price;
        order.qty        = qty;
        order.timestamp  = current_ts_;
        order.order_type = post_only ? OrderType::PostOnly : OrderType::Limit;
        order.client_id  = client_id;

        int64_t deliver_at = current_ts_ + latency_.order_rtt() / 2;
        order_queue_.push(deliver_at, OrderAckEvt{std::move(order), true});
        return oid;
    }

    std::string submit_market(const std::string& symbol,
                              Side side, double qty,
                              const std::string& client_id = "")
    {
        std::string oid = next_order_id();
        Order order;
        order.order_id   = oid;
        order.symbol     = symbol;
        order.side       = side;
        order.price      = 0.0;
        order.qty        = qty;
        order.timestamp  = current_ts_;
        order.order_type = OrderType::Market;
        order.client_id  = client_id;

        int64_t deliver_at = current_ts_ + latency_.order_rtt() / 2;
        order_queue_.push(deliver_at, OrderAckEvt{std::move(order), true});
        return oid;
    }

    bool cancel(const std::string& order_id) {
        auto it = open_orders_.find(order_id);
        if (it == open_orders_.end()) return false;
        Order copy = it->second;
        int64_t deliver_at = current_ts_ + latency_.cancel_rtt() / 2;
        order_queue_.push(deliver_at, OrderAckEvt{std::move(copy), false});
        return true;
    }

    int cancel_all(const std::string& symbol = "") {
        int n = 0;
        for (auto& [oid, order] : open_orders_) {
            if (symbol.empty() || order.symbol == symbol) {
                Order copy = order;
                int64_t deliver_at = current_ts_ + latency_.cancel_rtt() / 2;
                order_queue_.push(deliver_at, OrderAckEvt{std::move(copy), false});
                ++n;
            }
        }
        return n;
    }

    // ── Main simulation loop ──────────────────────────────────────────────────

    template<typename EventRange>
    SimStats run(EventRange&& events) {
        safe_call([this]{ strategy_.on_start(*this); });

        wall_start_ns_ = std::chrono::high_resolution_clock::now();
        next_snap_  = 0;
        tick_count_ = 0;

        for (auto& raw_event : events) {
            ++tick_count_;
            int64_t ts = event_timestamp(raw_event);
            current_ts_ = ts;

            drain_order_queue(ts);
            apply_market_event(raw_event, ts);
            int64_t feed_at = ts + latency_.feed_delay();
            schedule_feed(raw_event, feed_at);
            drain_feed_queue(ts);

            if (ts >= next_snap_) {
                auto mids = collect_mids();
                portfolio_.snapshot(ts, mids);

                if (!portfolio_.halted()) {
                    auto breaches = portfolio_.check_risk(mids);
                    if (!breaches.empty()) {
                        safe_call([&]{ strategy_.on_risk_breach(breaches, ts, *this); });
                        if (portfolio_.limits().halt_on_breach) {
                            portfolio_.set_halted(true);
                            cancel_all();
                        }
                    }
                }
                next_snap_ = ts + snapshot_interval_ns_;
            }
        }

        int64_t flush_ts = current_ts_ + 1'000'000'000'000LL;
        drain_order_queue(flush_ts);
        drain_feed_queue(flush_ts);

        safe_call([this]{ strategy_.on_end(*this); });

        auto wall_end = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(wall_end - wall_start_ns_).count();

        SimStats s;
        s.ticks_processed   = tick_count_;
        s.wall_time_s       = elapsed;
        s.ticks_per_second  = elapsed > 0.0 ? tick_count_ / elapsed : 0.0;
        s.total_fills       = static_cast<int64_t>(fill_history_.size());
        s.maker_fills       = 0; s.taker_fills = 0;
        for (auto& f : fill_history_) {
            if (f.is_maker) ++s.maker_fills; else ++s.taker_fills;
        }
        s.strategy_errors   = strategy_errors_;
        s.open_orders_final = static_cast<int64_t>(open_orders_.size());
        s.halted            = portfolio_.halted();
        s.portfolio_summary = portfolio_.summary();
        s.feed_latency      = latency_.feed_percentiles();
        s.order_latency     = latency_.order_percentiles();
        return s;
    }

    // ── Manual-drive API (for live replay) ───────────────────────────────────
    // Use these instead of run() when driving the engine event-by-event from
    // an external source (e.g., a live WebSocket feed thread).

    void on_start_manual() {
        wall_start_ns_ = std::chrono::high_resolution_clock::now();
        next_snap_     = 0;
        tick_count_    = 0;
        safe_call([this]{ strategy_.on_start(*this); });
    }

    void process_one(const MarketEvent& raw_event) {
        ++tick_count_;
        int64_t ts  = event_timestamp(raw_event);
        current_ts_ = ts;

        drain_order_queue(ts);
        apply_market_event(raw_event, ts);
        int64_t feed_at = ts + latency_.feed_delay();
        schedule_feed(raw_event, feed_at);
        drain_feed_queue(ts);

        if (ts >= next_snap_) {
            auto mids = collect_mids();
            portfolio_.snapshot(ts, mids);
            if (!portfolio_.halted()) {
                auto breaches = portfolio_.check_risk(mids);
                if (!breaches.empty()) {
                    safe_call([&]{ strategy_.on_risk_breach(breaches, ts, *this); });
                    if (portfolio_.limits().halt_on_breach) {
                        portfolio_.set_halted(true);
                        cancel_all();
                    }
                }
            }
            next_snap_ = ts + snapshot_interval_ns_;
        }
    }

    SimStats on_end_manual() {
        int64_t flush_ts = current_ts_ + 1'000'000'000'000LL;
        drain_order_queue(flush_ts);
        drain_feed_queue(flush_ts);
        safe_call([this]{ strategy_.on_end(*this); });

        auto wall_end = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(wall_end - wall_start_ns_).count();

        SimStats s;
        s.ticks_processed   = tick_count_;
        s.wall_time_s       = elapsed;
        s.ticks_per_second  = elapsed > 0.0 ? tick_count_ / elapsed : 0.0;
        s.total_fills       = static_cast<int64_t>(fill_history_.size());
        s.maker_fills       = 0; s.taker_fills = 0;
        for (auto& f : fill_history_) {
            if (f.is_maker) ++s.maker_fills; else ++s.taker_fills;
        }
        s.strategy_errors   = strategy_errors_;
        s.open_orders_final = static_cast<int64_t>(open_orders_.size());
        s.halted            = portfolio_.halted();
        s.portfolio_summary = portfolio_.summary();
        s.feed_latency      = latency_.feed_percentiles();
        s.order_latency     = latency_.order_percentiles();
        return s;
    }

    // Expose raw PnL series for analytics
    const std::vector<std::pair<int64_t,double>>& pnl_series() const {
        return portfolio_.pnl_series();
    }

    // Expose fill history for analytics
    const std::vector<FillEvent>& fill_history() const { return fill_history_; }

    OrderBook*       get_book(const std::string& sym) {
        auto it = books_.find(sym);
        return it != books_.end() ? it->second.get() : nullptr;
    }

    PortfolioState&  portfolio()    noexcept { return portfolio_; }
    int64_t          current_ts()   const noexcept { return current_ts_; }

    // Raw latency samples for analytics histograms
    const std::vector<int64_t>& latency_raw_feed()  const { return latency_.raw_feed_samples();  }
    const std::vector<int64_t>& latency_raw_order() const { return latency_.raw_order_samples(); }

    // Direct read access to open orders (for strategy inspection)
    const std::unordered_map<std::string, Order>& open_orders() const {
        return open_orders_;
    }

private:
    Strategy&           strategy_;
    LatencyModel        latency_;
    FillSimulator       fill_sim_;
    PortfolioState      portfolio_;
    int64_t             snapshot_interval_ns_;

    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books_;
    std::unordered_map<std::string, Order>                      open_orders_;
    std::vector<FillEvent>                                      fill_history_;

    TimedEventQueue feed_queue_;
    TimedEventQueue order_queue_;

    int64_t current_ts_       = 0;
    int64_t order_id_counter_ = 0;
    int64_t strategy_errors_  = 0;

    // Manual-drive state (also used internally by run())
    std::chrono::high_resolution_clock::time_point wall_start_ns_;
    int64_t next_snap_   = 0;
    int64_t tick_count_  = 0;

    // ── Internal helpers ──────────────────────────────────────────────────────

    std::string next_order_id() {
        return "o" + std::to_string(++order_id_counter_);
    }

    std::unordered_map<std::string, double> collect_mids() const {
        std::unordered_map<std::string, double> m;
        for (auto& [sym, book] : books_) {
            auto mp = book->mid_price();
            if (mp) m[sym] = *mp;
        }
        return m;
    }

    void apply_market_event(const MarketEvent& evt, int64_t ts) {
        std::visit([&](const auto& e) { apply_event_impl(e, ts); }, evt);
    }

    void apply_event_impl(const L2Update& upd, int64_t) {
        auto it = books_.find(upd.symbol);
        if (it != books_.end()) it->second->apply_l2(upd);
    }

    void apply_event_impl(const L3Update& upd, int64_t ts) {
        auto it = books_.find(upd.symbol);
        if (it == books_.end()) return;
        it->second->apply_l3(upd);
        if (upd.event == L3Event::Trade) {
            Trade t;
            t.trade_id  = "l3_" + std::to_string(ts);
            t.symbol    = upd.symbol;
            t.side      = upd.side;
            t.price     = upd.price;
            t.qty       = upd.qty;
            t.timestamp = upd.timestamp;
            t.aggressor = upd.side;
            check_fills(t, *it->second);
        }
    }

    void apply_event_impl(const Trade& trade, int64_t) {
        auto it = books_.find(trade.symbol);
        if (it == books_.end()) return;
        it->second->record_trade(trade);
        check_fills(trade, *it->second);
    }

    void check_fills(const Trade& trade, OrderBook& book) {
        if (open_orders_.empty() || portfolio_.halted()) return;

        double toxicity = 1.0 - book.trade_flow_ratio(200);

        // Build pointer map for fill simulator
        std::unordered_map<std::string, Order*> ptrs;
        ptrs.reserve(open_orders_.size());
        for (auto& [oid, order] : open_orders_) {
            ptrs[oid] = &order;
        }

        auto fills = fill_sim_.on_public_trade(trade, ptrs, book, toxicity);

        for (auto& fill : fills) {
            portfolio_.update_on_fill(fill);
            fill_history_.push_back(fill);

            int64_t deliver_at = current_ts_ + latency_.order_rtt() / 2;
            order_queue_.push(deliver_at, FillDelivEvt{fill});

            // Remove filled orders
            auto it = open_orders_.find(fill.order_id);
            if (it != open_orders_.end() &&
                it->second.status == OrderStatus::Filled) {
                open_orders_.erase(it);
            }
        }
    }

    void drain_order_queue(int64_t up_to_ns) {
        for (auto& evt : order_queue_.pop_ready(up_to_ns)) {
            std::visit([&](auto& e) { handle_order_event(e); }, evt.payload);
        }
    }

    void handle_order_event(OrderAckEvt& evt) {
        Order& order = evt.order;
        auto*  book  = get_book(order.symbol);

        if (!evt.accepted) {
            // Cancel path
            auto it = open_orders_.find(order.order_id);
            if (it != open_orders_.end()) {
                if (book) book->cancel_our_order(it->second);
                open_orders_.erase(it);
            }
            return;
        }

        if (order.order_type == OrderType::Market) {
            if (book) {
                double toxicity = 1.0 - book->trade_flow_ratio(100);
                auto fills = fill_sim_.fill_market_order(order, *book, current_ts_, toxicity);
                for (auto& fill : fills) {
                    portfolio_.update_on_fill(fill);
                    fill_history_.push_back(fill);
                    order_queue_.push(current_ts_ + latency_.order_rtt() / 2,
                                      FillDelivEvt{fill});
                }
            }
            order_history_.push_back(order);
            return;
        }

        // Limit / post_only
        if (order.order_type == OrderType::PostOnly && book) {
            if (!fill_sim_.check_post_only(order, *book)) {
                order.status = OrderStatus::Rejected;
                order_history_.push_back(order);
                if (!portfolio_.halted()) {
                    safe_call([&]{ strategy_.on_order_ack(order, false, current_ts_, *this); });
                }
                return;
            }
        }

        if (book) book->register_our_order(order);
        open_orders_[order.order_id] = order;
        order_history_.push_back(order);

        if (!portfolio_.halted()) {
            safe_call([&]{ strategy_.on_order_ack(order, true, current_ts_, *this); });
        }
    }

    void handle_order_event(FillDelivEvt& evt) {
        if (!portfolio_.halted()) {
            safe_call([&]{
                strategy_.on_fill(evt.fill, portfolio_, current_ts_, *this);
            });
        }
    }

    // Fallback for unhandled variant types
    template<typename T>
    void handle_order_event(T&) {}

    void schedule_feed(const MarketEvent& raw, int64_t feed_at) {
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, L2Update>)
                feed_queue_.push(feed_at, BookUpdateEvt{e});
            else if constexpr (std::is_same_v<T, L3Update>)
                feed_queue_.push(feed_at, BookUpdateL3Evt{e});
            else if constexpr (std::is_same_v<T, Trade>)
                feed_queue_.push(feed_at, TradeEvt{e});
        }, raw);
    }

    void drain_feed_queue(int64_t up_to_ns) {
        if (portfolio_.halted()) {
            feed_queue_.pop_ready(up_to_ns);  // drain but suppress callbacks
            return;
        }
        for (auto& evt : feed_queue_.pop_ready(up_to_ns)) {
            std::visit([&](const auto& e) { deliver_to_strategy(e); }, evt.payload);
        }
    }

    void deliver_to_strategy(const BookUpdateEvt& evt) {
        const std::string& sym = evt.update.symbol;
        auto* book = get_book(sym);
        if (book) safe_call([&]{
            strategy_.on_book_update(sym, *book, current_ts_, *this);
        });
    }
    void deliver_to_strategy(const BookUpdateL3Evt& evt) {
        const std::string& sym = evt.update.symbol;
        auto* book = get_book(sym);
        if (book) safe_call([&]{
            strategy_.on_book_update(sym, *book, current_ts_, *this);
        });
    }
    void deliver_to_strategy(const TradeEvt& evt) {
        const std::string& sym = evt.trade.symbol;
        auto* book = get_book(sym);
        if (book) safe_call([&]{
            strategy_.on_trade(evt.trade, *book, current_ts_, *this);
        });
    }
    template<typename T>
    void deliver_to_strategy(const T&) {}

    template<typename Fn>
    void safe_call(Fn&& fn) {
        try {
            fn();
        } catch (const std::exception& ex) {
            ++strategy_errors_;
            std::cerr << "[SimEngine] Strategy exception: " << ex.what() << "\n";
        } catch (...) {
            ++strategy_errors_;
            std::cerr << "[SimEngine] Unknown strategy exception\n";
        }
    }

    std::vector<Order> order_history_;
};

} // namespace hft