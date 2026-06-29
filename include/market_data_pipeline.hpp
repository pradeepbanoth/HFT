#pragma once
// market_data_pipeline.hpp — production-style market data validation pipeline

#include "types.hpp"
#include "simulator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hft {

enum class MdAction : uint8_t { Accept, Drop, Buffer, ResyncRequired };
enum class MdReason : uint8_t {
    None,
    Duplicate,
    OldSequence,
    GapDetected,
    OutOfOrderBuffered,
    BadTimestamp,
    UnknownSymbol,
    StaleEvent,
    BufferOverflow
};

struct MdDecision {
    MdAction action = MdAction::Accept;
    MdReason reason = MdReason::None;
};

struct MdStats {
    int64_t received = 0;
    int64_t accepted = 0;
    int64_t dropped = 0;
    int64_t buffered = 0;
    int64_t flushed = 0;
    int64_t duplicates = 0;
    int64_t gaps = 0;
    int64_t stale = 0;
    int64_t unknown_symbol = 0;
    int64_t resync_required = 0;
};

struct MdLatencyStats {
    int64_t samples = 0;
    double ewma_us = 0.0;
    double max_us = 0.0;
};

struct MdSymbolState {
    std::string symbol;
    int64_t last_seq = 0;
    int64_t last_ts = 0;
    int64_t last_receive_ts = 0;
    bool needs_resync = false;
};

class DuplicateFilter {
public:
    explicit DuplicateFilter(size_t max_keys = 200'000)
        : max_keys_(max_keys) {}

    bool seen(const MarketEvent& e) {
        std::string k = event_key(e);
        if (keys_.count(k)) return true;

        keys_.insert(k);
        fifo_.push_back(k);

        if (fifo_.size() > max_keys_) {
            keys_.erase(fifo_.front());
            fifo_.pop_front();
        }
        return false;
    }

    void clear() {
        keys_.clear();
        fifo_.clear();
    }

private:
    size_t max_keys_;
    std::unordered_set<std::string> keys_;
    std::deque<std::string> fifo_;

    static std::string event_key(const MarketEvent& e) {
        return std::visit([](const auto& ev) {
            std::string id;
            if constexpr (requires { ev.trade_id; }) id = ev.trade_id;
            else if constexpr (requires { ev.order_id; }) id = ev.order_id;

            return ev.symbol + "|" +
                   std::to_string(ev.timestamp) + "|" +
                   std::to_string(ev.price) + "|" +
                   std::to_string(ev.qty) + "|" +
                   id;
        }, e);
    }
};

class SymbolDispatcher {
public:
    using Callback = std::function<void(const MarketEvent&)>;

    void subscribe(std::string symbol, Callback cb) {
        callbacks_[std::move(symbol)].push_back(std::move(cb));
    }

    bool subscribed(const std::string& symbol) const {
        return callbacks_.count(symbol) > 0;
    }

    int dispatch(const MarketEvent& e) const {
        auto sym = symbol_of(e);
        auto it = callbacks_.find(sym);
        if (it == callbacks_.end()) return 0;

        int n = 0;
        for (const auto& cb : it->second) {
            cb(e);
            ++n;
        }
        return n;
    }

private:
    std::unordered_map<std::string, std::vector<Callback>> callbacks_;

    static std::string symbol_of(const MarketEvent& e) {
        return std::visit([](const auto& ev) { return ev.symbol; }, e);
    }
};

class MarketDataPipeline {
public:
    using ResyncCallback = std::function<void(const std::string&, int64_t)>;
    using DropCallback = std::function<void(const MarketEvent&, MdReason)>;

    struct Config {
        int64_t max_staleness_ns = 5'000'000'000LL;
        size_t max_buffered_per_symbol = 1024;
        bool buffer_out_of_order = true;
        bool strict_sequence = true;
    };

    MarketDataPipeline()
    : cfg_(Config{}) {}

    explicit MarketDataPipeline(Config cfg)
    : cfg_(cfg) {}
        

    void subscribe(const std::string& symbol, SymbolDispatcher::Callback cb) {
        dispatcher_.subscribe(symbol, std::move(cb));
        states_[symbol].symbol = symbol;
    }

    void on_resync_required(ResyncCallback cb) {
        resync_cb_ = std::move(cb);
    }

    void on_drop(DropCallback cb) {
        drop_cb_ = std::move(cb);
    }

    MdDecision ingest(const MarketEvent& event, int64_t receive_ts_ns = now_ns()) {
        ++stats_.received;

        const std::string symbol = symbol_of(event);
        const int64_t ts = event_timestamp(event);
        const int64_t seq = sequence_of(event);

        if (!dispatcher_.subscribed(symbol)) {
            ++stats_.dropped;
            ++stats_.unknown_symbol;
            emit_drop(event, MdReason::UnknownSymbol);
            return {MdAction::Drop, MdReason::UnknownSymbol};
        }

        if (ts <= 0) {
            ++stats_.dropped;
            emit_drop(event, MdReason::BadTimestamp);
            return {MdAction::Drop, MdReason::BadTimestamp};
        }

        auto& st = states_[symbol];

        if (receive_ts_ns - ts > cfg_.max_staleness_ns) {
            ++stats_.dropped;
            ++stats_.stale;
            emit_drop(event, MdReason::StaleEvent);
            return {MdAction::Drop, MdReason::StaleEvent};
        }

        update_latency(ts, receive_ts_ns);

        if (dupes_.seen(event)) {
            ++stats_.dropped;
            ++stats_.duplicates;
            emit_drop(event, MdReason::Duplicate);
            return {MdAction::Drop, MdReason::Duplicate};
        }

        if (cfg_.strict_sequence && seq > 0) {
            auto sd = check_sequence(symbol, st, seq, event);
            if (sd.action != MdAction::Accept) return sd;
        }

        accept_event(symbol, event, receive_ts_ns);
        flush_buffer(symbol, receive_ts_ns);

        return {};
    }

    const MdStats& stats() const noexcept { return stats_; }
    MdLatencyStats latency_stats() const noexcept { return latency_; }

    std::optional<MdSymbolState> state(const std::string& symbol) const {
        auto it = states_.find(symbol);
        if (it == states_.end()) return std::nullopt;
        return it->second;
    }

    void mark_resynced(const std::string& symbol, int64_t new_seq = 0) {
        auto& st = states_[symbol];
        st.needs_resync = false;
        if (new_seq > 0) st.last_seq = new_seq;
        buffers_[symbol].clear();
    }

private:
    struct BufferedEvent {
        int64_t seq = 0;
        int64_t ts = 0;
        MarketEvent event;
    };

    Config cfg_;
    MdStats stats_;
    MdLatencyStats latency_;
    DuplicateFilter dupes_;
    SymbolDispatcher dispatcher_;
    ResyncCallback resync_cb_;
    DropCallback drop_cb_;

    std::unordered_map<std::string, MdSymbolState> states_;
    std::unordered_map<std::string, std::vector<BufferedEvent>> buffers_;

    static int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    static std::string symbol_of(const MarketEvent& e) {
        return std::visit([](const auto& ev) { return ev.symbol; }, e);
    }

    static int64_t sequence_of(const MarketEvent& e) {
        return std::visit([](const auto& ev) {
            if constexpr (requires { ev.seq; }) return ev.seq;
            else return int64_t{0};
        }, e);
    }

    void update_latency(int64_t exchange_ts, int64_t recv_ts) {
        double us = std::max(0.0, static_cast<double>(recv_ts - exchange_ts) / 1000.0);
        latency_.samples++;
        latency_.ewma_us = latency_.samples == 1 ? us : latency_.ewma_us * 0.99 + us * 0.01;
        latency_.max_us = std::max(latency_.max_us, us);
    }

    MdDecision check_sequence(
        const std::string& symbol,
        MdSymbolState& st,
        int64_t seq,
        const MarketEvent& event
    ) {
        if (st.last_seq == 0) {
            st.last_seq = seq - 1;
        }

        if (seq <= st.last_seq) {
            ++stats_.dropped;
            ++stats_.duplicates;
            emit_drop(event, MdReason::OldSequence);
            return {MdAction::Drop, MdReason::OldSequence};
        }

        if (seq > st.last_seq + 1) {
            if (cfg_.buffer_out_of_order) {
                auto& buf = buffers_[symbol];

                if (buf.size() >= cfg_.max_buffered_per_symbol) {
                    ++stats_.dropped;
                    emit_drop(event, MdReason::BufferOverflow);
                    return {MdAction::Drop, MdReason::BufferOverflow};
                }

                buf.push_back({seq, event_timestamp(event), event});
                ++stats_.buffered;
                ++stats_.gaps;
                st.needs_resync = true;

                if (resync_cb_) resync_cb_(symbol, st.last_seq + 1);
                return {MdAction::Buffer, MdReason::OutOfOrderBuffered};
            }

            ++stats_.resync_required;
            ++stats_.gaps;
            st.needs_resync = true;
            if (resync_cb_) resync_cb_(symbol, st.last_seq + 1);
            return {MdAction::ResyncRequired, MdReason::GapDetected};
        }

        return {};
    }

    void accept_event(const std::string& symbol, const MarketEvent& event, int64_t recv_ts) {
        auto& st = states_[symbol];
        int64_t seq = sequence_of(event);

        if (seq > 0) st.last_seq = seq;
        st.last_ts = event_timestamp(event);
        st.last_receive_ts = recv_ts;

        dispatcher_.dispatch(event);
        ++stats_.accepted;
    }

    void flush_buffer(const std::string& symbol, int64_t recv_ts) {
        auto& buf = buffers_[symbol];
        if (buf.empty()) return;

        std::sort(buf.begin(), buf.end(),
            [](const BufferedEvent& a, const BufferedEvent& b) {
                return a.seq < b.seq;
            });

        bool progressed = true;
        while (progressed) {
            progressed = false;
            auto& st = states_[symbol];

            for (auto it = buf.begin(); it != buf.end();) {
                if (it->seq == st.last_seq + 1) {
                    MarketEvent e = it->event;
                    it = buf.erase(it);
                    accept_event(symbol, e, recv_ts);
                    ++stats_.flushed;
                    progressed = true;
                } else {
                    ++it;
                }
            }
        }
    }

    void emit_drop(const MarketEvent& e, MdReason r) {
        if (drop_cb_) drop_cb_(e, r);
    }
};

} // namespace hft