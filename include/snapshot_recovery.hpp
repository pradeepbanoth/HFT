#pragma once
// snapshot_recovery.hpp — advanced snapshot recovery + resync state machine

#include "types.hpp"
#include "orderbook.hpp"
#include "simulator.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

enum class RecoveryState : uint8_t {
    Synced,
    GapDetected,
    SnapshotRequested,
    Rebuilding,
    ReplayingBuffer,
    Failed
};

enum class RecoveryReason : uint8_t {
    None,
    Gap,
    SnapshotTimeout,
    BufferOverflow,
    StaleSnapshot,
    ReplayFailed
};

struct BookSnapshot {
    std::string symbol;
    int64_t snapshot_seq = 0;
    int64_t timestamp = 0;
    std::vector<DepthLevel> bids;
    std::vector<DepthLevel> asks;
};

struct SnapshotRequest {
    std::string symbol;
    int64_t expected_seq = 0;
    int64_t request_ts = 0;
    int64_t attempt = 0;
};

struct RecoveryStats {
    int64_t gaps_detected = 0;
    int64_t snapshots_requested = 0;
    int64_t snapshots_applied = 0;
    int64_t snapshots_rejected = 0;
    int64_t buffered_events = 0;
    int64_t replayed_events = 0;
    int64_t failed_recoveries = 0;
    int64_t timeouts = 0;
};

struct RecoveryReport {
    std::string symbol;
    RecoveryState state = RecoveryState::Synced;
    RecoveryReason reason = RecoveryReason::None;
    int64_t expected_seq = 0;
    int64_t last_snapshot_seq = 0;
    int64_t buffered = 0;
    int64_t attempts = 0;
};

class SnapshotRecoveryManager {
public:
    using SnapshotRequester = std::function<void(const SnapshotRequest&)>;
    using ReplayCallback = std::function<bool(const MarketEvent&)>;
    using StateCallback = std::function<void(const RecoveryReport&)>;

    struct Config {
        size_t max_buffered_events = 100'000;
        int64_t request_timeout_ns = 5'000'000'000LL;
        int64_t min_request_interval_ns = 250'000'000LL;
        int64_t max_attempts = 5;
        bool reject_stale_snapshot = true;
    };

    explicit SnapshotRecoveryManager()
    : cfg_(Config{})
{
}

explicit SnapshotRecoveryManager(const Config& cfg)
    : cfg_(cfg)
{
}

    void set_requester(SnapshotRequester cb) {
        requester_ = std::move(cb);
    }

    void set_replay_callback(ReplayCallback cb) {
        replay_cb_ = std::move(cb);
    }

    void set_state_callback(StateCallback cb) {
        state_cb_ = std::move(cb);
    }

    void on_gap(const std::string& symbol, int64_t expected_seq, int64_t now_ts) {
        auto& st = states_[symbol];

        if (st.state == RecoveryState::Synced) {
            ++stats_.gaps_detected;
            st.expected_seq = expected_seq;
            transition(symbol, RecoveryState::GapDetected, RecoveryReason::Gap);
        }

        request_snapshot(symbol, now_ts);
    }

    bool should_buffer(const std::string& symbol) const {
        auto s = state(symbol);
        return s == RecoveryState::GapDetected ||
               s == RecoveryState::SnapshotRequested ||
               s == RecoveryState::Rebuilding ||
               s == RecoveryState::ReplayingBuffer;
    }

    bool buffer_event(const MarketEvent& event) {
        const std::string sym = symbol_of(event);
        auto& st = states_[sym];

        if (st.buffer.size() >= cfg_.max_buffered_events) {
            transition(sym, RecoveryState::Failed, RecoveryReason::BufferOverflow);
            ++stats_.failed_recoveries;
            return false;
        }

        st.buffer.push_back({sequence_of(event), event_timestamp(event), event});
        ++stats_.buffered_events;
        return true;
    }

    bool apply_snapshot(const BookSnapshot& snap, OrderBook& book, int64_t now_ts = 0) {
        auto& st = states_[snap.symbol];

        if (cfg_.reject_stale_snapshot && snap.snapshot_seq < st.expected_seq - 1) {
            ++stats_.snapshots_rejected;
            transition(snap.symbol, RecoveryState::SnapshotRequested, RecoveryReason::StaleSnapshot);
            request_snapshot(snap.symbol, now_ts);
            return false;
        }

        transition(snap.symbol, RecoveryState::Rebuilding, RecoveryReason::None);

        book.clear();
        for (const auto& lv : snap.bids) {
            book.apply_l2({snap.symbol, BookSide::Bid, lv.price, lv.qty, snap.timestamp, snap.snapshot_seq});
        }
        for (const auto& lv : snap.asks) {
            book.apply_l2({snap.symbol, BookSide::Ask, lv.price, lv.qty, snap.timestamp, snap.snapshot_seq});
        }

        st.last_snapshot_seq = snap.snapshot_seq;

        transition(snap.symbol, RecoveryState::ReplayingBuffer, RecoveryReason::None);

        if (!replay_buffer(snap.symbol, snap.snapshot_seq)) {
            transition(snap.symbol, RecoveryState::Failed, RecoveryReason::ReplayFailed);
            ++stats_.failed_recoveries;
            return false;
        }

        st.buffer.clear();
        st.expected_seq = snap.snapshot_seq;
        transition(snap.symbol, RecoveryState::Synced, RecoveryReason::None);
        ++stats_.snapshots_applied;
        return true;
    }


   

    void tick(int64_t now_ts) {
        for (auto& [sym, st] : states_) {
            if (st.state != RecoveryState::SnapshotRequested) continue;

            if (now_ts - st.last_request_ts > cfg_.request_timeout_ns) {
                ++stats_.timeouts;

                if (st.attempts >= cfg_.max_attempts) {
                    transition(sym, RecoveryState::Failed, RecoveryReason::SnapshotTimeout);
                    ++stats_.failed_recoveries;
                } else {
                    request_snapshot(sym, now_ts);
                }
            }
        }
    }

    void mark_synced(const std::string& symbol, int64_t seq = 0) {
        auto& st = states_[symbol];
        if (seq > 0) st.expected_seq = seq;
        st.buffer.clear();
        transition(symbol, RecoveryState::Synced, RecoveryReason::None);
    }

    void reset(const std::string& symbol) {
        states_.erase(symbol);
    }

    RecoveryState state(const std::string& symbol) const {
        auto it = states_.find(symbol);
        return it == states_.end() ? RecoveryState::Synced : it->second.state;
    }

    RecoveryReport report(const std::string& symbol) const {
        RecoveryReport r;
        r.symbol = symbol;

        auto it = states_.find(symbol);
        if (it == states_.end()) return r;

        const auto& st = it->second;
        r.state = st.state;
        r.reason = st.reason;
        r.expected_seq = st.expected_seq;
        r.last_snapshot_seq = st.last_snapshot_seq;
        r.buffered = static_cast<int64_t>(st.buffer.size());
        r.attempts = st.attempts;
        return r;
    }

    const RecoveryStats& stats() const noexcept {
        return stats_;
    }

private:
    struct BufferedEvent {
        int64_t seq = 0;
        int64_t ts = 0;
        MarketEvent event;
    };

    struct SymbolRecovery {
        RecoveryState state = RecoveryState::Synced;
        RecoveryReason reason = RecoveryReason::None;
        int64_t expected_seq = 0;
        int64_t last_snapshot_seq = 0;
        int64_t last_request_ts = 0;
        int64_t attempts = 0;
        std::deque<BufferedEvent> buffer;
    };

    Config cfg_;
    std::unordered_map<std::string, SymbolRecovery> states_;
    SnapshotRequester requester_;
    ReplayCallback replay_cb_;
    StateCallback state_cb_;
    RecoveryStats stats_;

    void request_snapshot(const std::string& symbol, int64_t now_ts) {
        auto& st = states_[symbol];

        if (now_ts > 0 &&
            st.last_request_ts > 0 &&
            now_ts - st.last_request_ts < cfg_.min_request_interval_ns) {
            return;
        }

        st.last_request_ts = now_ts;
        ++st.attempts;
        ++stats_.snapshots_requested;

        transition(symbol, RecoveryState::SnapshotRequested, RecoveryReason::Gap);

        if (requester_) {
            requester_({
                symbol,
                st.expected_seq,
                now_ts,
                st.attempts
            });
        }
    }

    bool replay_buffer(const std::string& symbol, int64_t snapshot_seq) {
        auto& st = states_[symbol];

        std::vector<BufferedEvent> events;
        events.reserve(st.buffer.size());

        for (const auto& e : st.buffer) {
            if (e.seq == 0 || e.seq > snapshot_seq)
                events.push_back(e);
        }

        std::stable_sort(events.begin(), events.end(),
            [](const BufferedEvent& a, const BufferedEvent& b) {
                if (a.seq != b.seq) return a.seq < b.seq;
                return a.ts < b.ts;
            });

        for (const auto& e : events) {
            if (replay_cb_) {
                if (!replay_cb_(e.event)) return false;
            }
            ++stats_.replayed_events;
        }

        return true;
    }

    void transition(const std::string& symbol, RecoveryState s, RecoveryReason r) {
        auto& st = states_[symbol];
        st.state = s;
        st.reason = r;

        if (state_cb_) state_cb_(report(symbol));
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
};

} // namespace hft