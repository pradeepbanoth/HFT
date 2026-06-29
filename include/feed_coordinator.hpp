#pragma once
// feed_coordinator.hpp — production-grade feed coordination layer

#include "types.hpp"
#include "simulator.hpp"
#include "orderbook.hpp"
#include "market_data_pipeline.hpp"
#include "snapshot_recovery.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

enum class FeedSymbolStatus : uint8_t {
    Active,
    Recovering,
    Suspended,
    Failed
};

struct FeedSymbolReport {
    std::string symbol;
    FeedSymbolStatus status = FeedSymbolStatus::Active;
    int64_t ingested = 0;
    int64_t dispatched = 0;
    int64_t buffered = 0;
    int64_t dropped = 0;
    int64_t snapshots_requested = 0;
    int64_t snapshots_applied = 0;
    int64_t resyncs = 0;
};

struct FeedCoordinatorStats {
    int64_t events_ingested = 0;
    int64_t events_dispatched = 0;
    int64_t events_buffered = 0;
    int64_t events_dropped = 0;
    int64_t snapshots_requested = 0;
    int64_t snapshots_applied = 0;
    int64_t resyncs = 0;
    int64_t recovery_failures = 0;
};

class FeedCoordinator {
public:
    using StrategyCallback = std::function<void(const MarketEvent&)>;
    using SnapshotRequester = std::function<void(const SnapshotRequest&)>;
    using DropCallback = std::function<void(const MarketEvent&, MdReason)>;
    using StatusCallback = std::function<void(const FeedSymbolReport&)>;

    struct Config {
        MarketDataPipeline::Config pipeline;
        SnapshotRecoveryManager::Config recovery;
        size_t quarantine_limit = 10'000;
        bool quarantine_dropped = true;
    };

    FeedCoordinator()
    : cfg_(Config())
    , pipeline_(cfg_.pipeline)
    , recovery_(cfg_.recovery)
    {
    wire_callbacks();
    }

    explicit FeedCoordinator(const Config& cfg)
    : cfg_(cfg)
    , pipeline_(cfg_.pipeline)
    , recovery_(cfg_.recovery)
    {
    wire_callbacks();
    }

    void add_symbol(const std::string& symbol, OrderBook* book, StrategyCallback cb) {
        books_[symbol] = book;
        reports_[symbol].symbol = symbol;
        reports_[symbol].status = FeedSymbolStatus::Active;

        pipeline_.subscribe(symbol, [this, symbol, cb](const MarketEvent& e) {
            auto& r = reports_[symbol];
            ++r.dispatched;
            ++stats_.events_dispatched;
            cb(e);
        });
    }

    void suspend_symbol(const std::string& symbol) {
        reports_[symbol].symbol = symbol;
        reports_[symbol].status = FeedSymbolStatus::Suspended;
        emit_status(symbol);
    }

    void resume_symbol(const std::string& symbol) {
        reports_[symbol].symbol = symbol;
        reports_[symbol].status = FeedSymbolStatus::Active;
        pipeline_.mark_resynced(symbol);
        recovery_.mark_synced(symbol);
        emit_status(symbol);
    }

    void set_snapshot_requester(SnapshotRequester cb) {
        snapshot_requester_ = std::move(cb);
    }

    void on_drop(DropCallback cb) {
        drop_cb_ = std::move(cb);
    }

    void on_status(StatusCallback cb) {
        status_cb_ = std::move(cb);
    }

    MdDecision ingest(const MarketEvent& event, int64_t receive_ts_ns = now_ns()) {
        ++stats_.events_ingested;

        const std::string sym = symbol_of(event);
        auto& report = reports_[sym];
        report.symbol = sym;
        ++report.ingested;

        if (report.status == FeedSymbolStatus::Suspended ||
            report.status == FeedSymbolStatus::Failed) {
            ++stats_.events_dropped;
            ++report.dropped;
            quarantine(event, MdReason::UnknownSymbol);
            return {MdAction::Drop, MdReason::UnknownSymbol};
        }

        if (recovery_.should_buffer(sym)) {
            bool ok = recovery_.buffer_event(event);
            if (ok) {
                ++stats_.events_buffered;
                ++report.buffered;
                report.status = FeedSymbolStatus::Recovering;
                emit_status(sym);
                return {MdAction::Buffer, MdReason::OutOfOrderBuffered};
            }

            mark_failed(sym);
            quarantine(event, MdReason::BufferOverflow);
            return {MdAction::Drop, MdReason::BufferOverflow};
        }

        MdDecision d = pipeline_.ingest(event, receive_ts_ns);

        if (d.action == MdAction::Accept) {
            return d;
        }

        if (d.action == MdAction::Buffer || d.action == MdAction::ResyncRequired) {
            bool ok = recovery_.buffer_event(event);
            if (ok) {
                ++stats_.events_buffered;
                ++report.buffered;
                report.status = FeedSymbolStatus::Recovering;
                emit_status(sym);
            }
            return d;
        }

        ++stats_.events_dropped;
        ++report.dropped;
        quarantine(event, d.reason);
        return d;
    }

    bool apply_snapshot(const BookSnapshot& snap, int64_t now_ts = now_ns()) {
        auto bit = books_.find(snap.symbol);
        if (bit == books_.end() || !bit->second) return false;

        bool ok = recovery_.apply_snapshot(snap, *bit->second, now_ts);

        auto& r = reports_[snap.symbol];
        r.symbol = snap.symbol;

        if (!ok) {
            mark_failed(snap.symbol);
            return false;
        }

        pipeline_.mark_resynced(snap.symbol, snap.snapshot_seq);

        ++stats_.snapshots_applied;
        ++r.snapshots_applied;
        r.status = FeedSymbolStatus::Active;
        emit_status(snap.symbol);
        return true;
    }

    void tick(int64_t now_ts = now_ns()) {
        recovery_.tick(now_ts);

        for (auto& [sym, r] : reports_) {
            auto rr = recovery_.report(sym);
            if (rr.state == RecoveryState::Failed && r.status != FeedSymbolStatus::Failed) {
                mark_failed(sym);
            }
        }
    }

    std::optional<FeedSymbolReport> report(const std::string& symbol) const {
        auto it = reports_.find(symbol);
        if (it == reports_.end()) return std::nullopt;
        return it->second;
    }

    const FeedCoordinatorStats& stats() const noexcept {
        return stats_;
    }

    const MdStats& pipeline_stats() const noexcept {
        return pipeline_.stats();
    }

    const RecoveryStats& recovery_stats() const noexcept {
        return recovery_.stats();
    }

    RecoveryReport recovery_report(const std::string& symbol) const {
        return recovery_.report(symbol);
    }

    const std::deque<MarketEvent>& quarantine_events() const noexcept {
        return quarantine_;
    }

private:
    Config cfg_;
    MarketDataPipeline pipeline_;
    SnapshotRecoveryManager recovery_;

    std::unordered_map<std::string, OrderBook*> books_;
    std::unordered_map<std::string, FeedSymbolReport> reports_;

    SnapshotRequester snapshot_requester_;
    DropCallback drop_cb_;
    StatusCallback status_cb_;

    FeedCoordinatorStats stats_;
    std::deque<MarketEvent> quarantine_;

    void wire_callbacks() {
        pipeline_.on_resync_required([this](const std::string& symbol, int64_t expected_seq) {
            ++stats_.resyncs;

            auto& r = reports_[symbol];
            r.symbol = symbol;
            r.status = FeedSymbolStatus::Recovering;
            ++r.resyncs;

            recovery_.on_gap(symbol, expected_seq, now_ns());
            emit_status(symbol);
        });

        pipeline_.on_drop([this](const MarketEvent& e, MdReason reason) {
            if (drop_cb_) drop_cb_(e, reason);
        });

        recovery_.set_requester([this](const SnapshotRequest& req) {
            ++stats_.snapshots_requested;

            auto& r = reports_[req.symbol];
            r.symbol = req.symbol;
            ++r.snapshots_requested;
            r.status = FeedSymbolStatus::Recovering;

            emit_status(req.symbol);

            if (snapshot_requester_)
                snapshot_requester_(req);
        });

        recovery_.set_replay_callback([this](const MarketEvent& e) {
            const std::string sym = symbol_of(e);
            pipeline_.mark_resynced(sym, sequence_of(e));
            MdDecision d = pipeline_.ingest(e, now_ns());
            return d.action == MdAction::Accept;
        });

        recovery_.set_state_callback([this](const RecoveryReport& rr) {
            if (rr.state == RecoveryState::Failed) {
                mark_failed(rr.symbol);
            }
        });
    }

    void mark_failed(const std::string& symbol) {
        ++stats_.recovery_failures;

        auto& r = reports_[symbol];
        r.symbol = symbol;
        r.status = FeedSymbolStatus::Failed;
        emit_status(symbol);
    }

    void quarantine(const MarketEvent& e, MdReason reason) {
        if (drop_cb_) drop_cb_(e, reason);

        if (!cfg_.quarantine_dropped) return;

        quarantine_.push_back(e);
        while (quarantine_.size() > cfg_.quarantine_limit)
            quarantine_.pop_front();
    }

    void emit_status(const std::string& symbol) {
        if (!status_cb_) return;
        auto it = reports_.find(symbol);
        if (it != reports_.end())
            status_cb_(it->second);
    }

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
};

} // namespace hft