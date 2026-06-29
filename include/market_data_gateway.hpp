#pragma once
// market_data_gateway.hpp — advanced multi-venue market data gateway

#include "types.hpp"
#include "orderbook.hpp"
#include "feed_coordinator.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

enum class MdGatewayStatus : uint8_t {
    Running,
    Degraded,
    Stopped
};

enum class VenueFeedStatus : uint8_t {
    Active,
    Paused,
    Degraded,
    Failed
};

struct VenueFeedStats {
    std::string venue;
    VenueFeedStatus status = VenueFeedStatus::Active;

    FeedCoordinatorStats coordinator;
    MdStats pipeline;
    RecoveryStats recovery;

    int64_t events_ingested = 0;
    int64_t events_dropped = 0;
    int64_t events_dispatched = 0;
    int64_t snapshots_requested = 0;

    double health_score = 1.0;
};

struct GatewayAggregateStats {
    int64_t venues = 0;
    int64_t active_venues = 0;
    int64_t events_ingested = 0;
    int64_t events_dispatched = 0;
    int64_t events_dropped = 0;
    int64_t snapshots_requested = 0;
    double avg_health_score = 1.0;
};

class MarketDataGateway {
public:
    using EventCallback = std::function<void(const std::string& venue, const MarketEvent&)>;
    using SnapshotCallback = std::function<void(const std::string& venue, const SnapshotRequest&)>;
    using DropCallback = std::function<void(const std::string& venue, const MarketEvent&, MdReason)>;
    using VenueStatusCallback = std::function<void(const VenueFeedStats&)>;

    explicit MarketDataGateway(FeedCoordinator::Config cfg = {})
        : cfg_(std::move(cfg)) {}

    bool add_venue(const std::string& venue) {
        if (venue.empty()) return false;
        if (feeds_.count(venue)) return false;

        VenueCtx ctx;
        ctx.feed = std::make_unique<FeedCoordinator>(cfg_);
        ctx.venue = venue;
        ctx.stats.venue = venue;
        ctx.stats.status = VenueFeedStatus::Active;

        ctx.feed->set_snapshot_requester([this, venue](const SnapshotRequest& req) {
            auto* c = get_ctx(venue);
            if (c) {
                ++c->stats.snapshots_requested;
                ++aggregate_.snapshots_requested;
            }

            if (snapshot_cb_) snapshot_cb_(venue, req);
        });

        ctx.feed->on_drop([this, venue](const MarketEvent& e, MdReason r) {
            auto* c = get_ctx(venue);
            if (c) {
                ++c->stats.events_dropped;
                ++aggregate_.events_dropped;
                recompute_health(*c);
                emit_venue_status(*c);
            }

            if (drop_cb_) drop_cb_(venue, e, r);
        });

        feeds_[venue] = std::move(ctx);
        recompute_aggregate();
        return true;
    }

    bool remove_venue(const std::string& venue) {
        bool ok = feeds_.erase(venue) > 0;
        recompute_aggregate();
        return ok;
    }

    bool pause_venue(const std::string& venue) {
        auto* c = get_ctx(venue);
        if (!c) return false;
        c->stats.status = VenueFeedStatus::Paused;
        emit_venue_status(*c);
        recompute_aggregate();
        return true;
    }

    bool resume_venue(const std::string& venue) {
        auto* c = get_ctx(venue);
        if (!c) return false;
        c->stats.status = VenueFeedStatus::Active;
        emit_venue_status(*c);
        recompute_aggregate();
        return true;
    }

    bool mark_failed(const std::string& venue) {
        auto* c = get_ctx(venue);
        if (!c) return false;
        c->stats.status = VenueFeedStatus::Failed;
        c->stats.health_score = 0.0;
        emit_venue_status(*c);
        recompute_aggregate();
        return true;
    }

    void add_symbol(
        const std::string& venue,
        const std::string& symbol,
        OrderBook* book,
        EventCallback cb
    ) {
        add_venue(venue);

        auto* c = get_ctx(venue);
        if (!c || !book) return;

        c->books[symbol] = book;

        c->feed->add_symbol(symbol, book,
            [this, venue, cb](const MarketEvent& e) {
                auto* ctx = get_ctx(venue);
                if (ctx) {
                    ++ctx->stats.events_dispatched;
                    ++aggregate_.events_dispatched;
                }

                if (cb) cb(venue, e);
            }
        );

        emit_venue_status(*c);
    }

    void on_snapshot_request(SnapshotCallback cb) {
        snapshot_cb_ = std::move(cb);
    }

    void on_drop(DropCallback cb) {
        drop_cb_ = std::move(cb);
    }

    void on_venue_status(VenueStatusCallback cb) {
        venue_status_cb_ = std::move(cb);
    }

    MdDecision ingest(
        const std::string& venue,
        const MarketEvent& event,
        int64_t receive_ts_ns = now_ns()
    ) {
        auto* c = get_ctx(venue);
        if (!c) {
            ++aggregate_.events_dropped;
            return {MdAction::Drop, MdReason::UnknownSymbol};
        }

        if (stopped_ || c->stats.status == VenueFeedStatus::Paused ||
            c->stats.status == VenueFeedStatus::Failed) {
            ++c->stats.events_dropped;
            ++aggregate_.events_dropped;
            recompute_health(*c);
            emit_venue_status(*c);
            return {MdAction::Drop, MdReason::UnknownSymbol};
        }

        ++c->stats.events_ingested;
        ++aggregate_.events_ingested;

        MdDecision d = c->feed->ingest(event, receive_ts_ns);

        c->stats.coordinator = c->feed->stats();
        c->stats.pipeline = c->feed->pipeline_stats();
        c->stats.recovery = c->feed->recovery_stats();

        if (d.action == MdAction::Drop) {
            ++c->stats.events_dropped;
            ++aggregate_.events_dropped;
        }

        recompute_health(*c);
        emit_venue_status(*c);
        recompute_aggregate();
        return d;
    }

    bool apply_snapshot(
        const std::string& venue,
        const BookSnapshot& snap,
        int64_t ts = now_ns()
    ) {
        auto* c = get_ctx(venue);
        if (!c) return false;

        bool ok = c->feed->apply_snapshot(snap, ts);

        c->stats.coordinator = c->feed->stats();
        c->stats.pipeline = c->feed->pipeline_stats();
        c->stats.recovery = c->feed->recovery_stats();

        if (ok) {
            c->stats.status = VenueFeedStatus::Active;
        } else {
            c->stats.status = VenueFeedStatus::Degraded;
        }

        recompute_health(*c);
        emit_venue_status(*c);
        recompute_aggregate();
        return ok;
    }

    void tick(int64_t ts = now_ns()) {
        if (stopped_) return;

        for (auto& [venue, ctx] : feeds_) {
            if (ctx.stats.status == VenueFeedStatus::Paused ||
                ctx.stats.status == VenueFeedStatus::Failed) {
                continue;
            }

            ctx.feed->tick(ts);
            ctx.stats.coordinator = ctx.feed->stats();
            ctx.stats.pipeline = ctx.feed->pipeline_stats();
            ctx.stats.recovery = ctx.feed->recovery_stats();

            if (ctx.stats.recovery.failed_recoveries > 0)
                ctx.stats.status = VenueFeedStatus::Degraded;

            recompute_health(ctx);
            emit_venue_status(ctx);
        }

        recompute_aggregate();
    }

    std::optional<VenueFeedStats> venue_stats(const std::string& venue) const {
        auto it = feeds_.find(venue);
        if (it == feeds_.end()) return std::nullopt;
        return it->second.stats;
    }

    GatewayAggregateStats aggregate_stats() const {
        return aggregate_;
    }

    MdGatewayStatus status() const noexcept {
        if (stopped_) return MdGatewayStatus::Stopped;
        if (aggregate_.avg_health_score < 0.75 || aggregate_.events_dropped > 0)
            return MdGatewayStatus::Degraded;
        return MdGatewayStatus::Running;
    }

    void stop() noexcept {
        stopped_ = true;
        recompute_aggregate();
    }

    void start() noexcept {
        stopped_ = false;
        recompute_aggregate();
    }

private:
    struct VenueCtx {
        std::string venue;
        std::unique_ptr<FeedCoordinator> feed;
        std::unordered_map<std::string, OrderBook*> books;
        VenueFeedStats stats;
    };

    FeedCoordinator::Config cfg_;
    std::unordered_map<std::string, VenueCtx> feeds_;

    SnapshotCallback snapshot_cb_;
    DropCallback drop_cb_;
    VenueStatusCallback venue_status_cb_;

    GatewayAggregateStats aggregate_;
    bool stopped_ = false;

    VenueCtx* get_ctx(const std::string& venue) {
        auto it = feeds_.find(venue);
        return it == feeds_.end() ? nullptr : &it->second;
    }

    const VenueCtx* get_ctx(const std::string& venue) const {
        auto it = feeds_.find(venue);
        return it == feeds_.end() ? nullptr : &it->second;
    }

    static int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    static void recompute_health(VenueCtx& c) {
        double drop_penalty = 0.0;
        if (c.stats.events_ingested > 0) {
            drop_penalty = static_cast<double>(c.stats.events_dropped) /
                           static_cast<double>(c.stats.events_ingested);
        }

        double gap_penalty = c.stats.pipeline.gaps > 0 ? 0.15 : 0.0;
        double recovery_penalty = c.stats.recovery.failed_recoveries > 0 ? 0.5 : 0.0;

        c.stats.health_score = std::clamp(
            1.0 - drop_penalty - gap_penalty - recovery_penalty,
            0.0,
            1.0
        );

        if (c.stats.status == VenueFeedStatus::Active && c.stats.health_score < 0.75)
            c.stats.status = VenueFeedStatus::Degraded;
    }

    void recompute_aggregate() {
        aggregate_ = GatewayAggregateStats{};
        aggregate_.venues = static_cast<int64_t>(feeds_.size());

        double health_sum = 0.0;
        for (const auto& [venue, ctx] : feeds_) {
            if (ctx.stats.status == VenueFeedStatus::Active)
                ++aggregate_.active_venues;

            aggregate_.events_ingested += ctx.stats.events_ingested;
            aggregate_.events_dispatched += ctx.stats.events_dispatched;
            aggregate_.events_dropped += ctx.stats.events_dropped;
            aggregate_.snapshots_requested += ctx.stats.snapshots_requested;
            health_sum += ctx.stats.health_score;
        }

        aggregate_.avg_health_score = aggregate_.venues > 0
            ? health_sum / aggregate_.venues
            : 1.0;
    }

    void emit_venue_status(const VenueCtx& c) {
        if (venue_status_cb_) venue_status_cb_(c.stats);
    }
};

} // namespace hft