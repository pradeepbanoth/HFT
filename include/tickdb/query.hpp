#pragma once
// tickdb/query.hpp — advanced query engine for segmented tick database

#include "tickdb/metadata.hpp"
#include "tickdb/segment.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft::tickdb {

enum class TickSideFilter : uint8_t {
    Any,
    Buy,
    Sell,
    Bid,
    Ask
};

enum class QueryOrder : uint8_t {
    Asc,
    Desc
};

enum class QueryMode : uint8_t {
    Materialize,
    Stream
};

struct TickQuery {
    std::string exchange;
    std::string symbol;

    int64_t start_ts = std::numeric_limits<int64_t>::min();
    int64_t end_ts = std::numeric_limits<int64_t>::max();

    bool include_l2 = true;
    bool include_l3 = true;
    bool include_trades = true;

    TickSideFilter side = TickSideFilter::Any;

    double min_qty = 0.0;
    double max_qty = std::numeric_limits<double>::max();

    double min_price = 0.0;
    double max_price = std::numeric_limits<double>::max();

    size_t offset = 0;
    size_t limit = 0;

    QueryOrder order = QueryOrder::Asc;
    QueryMode mode = QueryMode::Materialize;

    bool verify_crc = true;
    bool allow_corrupt = false;
    bool require_indexed = false;
};

struct QueryStats {
    uint64_t candidate_segments = 0;
    uint64_t segments_scanned = 0;
    uint64_t segments_skipped = 0;

    uint64_t events_scanned = 0;
    uint64_t events_matched = 0;
    uint64_t events_returned = 0;

    int64_t first_ts = 0;
    int64_t last_ts = 0;

    double min_price = std::numeric_limits<double>::max();
    double max_price = 0.0;
    double total_qty = 0.0;
    double total_notional = 0.0;

    double vwap() const noexcept {
        return total_qty > 1e-12 ? total_notional / total_qty : 0.0;
    }
};

struct QueryPlan {
    SegmentQuery segment_query;
    std::vector<SegmentMetadata> segments;
    bool reverse = false;
};

struct QueryResult {
    std::vector<MarketEvent> events;
    QueryStats stats;
};

using TickVisitor = std::function<bool(const MarketEvent&)>;

class TickQueryEngine {
public:
    explicit TickQueryEngine(const MetadataCatalog& catalog)
        : catalog_(catalog) {}

    QueryPlan plan(const TickQuery& q) const {
        QueryPlan p;
        p.segment_query.exchange = q.exchange;
        p.segment_query.symbol = q.symbol;
        p.segment_query.start_ts = q.start_ts;
        p.segment_query.end_ts = q.end_ts;
        p.segment_query.include_corrupt = q.allow_corrupt;
        p.segment_query.require_indexed = q.require_indexed;

        p.segments = catalog_.query(p.segment_query);
        p.reverse = q.order == QueryOrder::Desc;

        if (p.reverse) {
            std::reverse(p.segments.begin(), p.segments.end());
        }

        return p;
    }

    QueryResult execute(const TickQuery& q) const {
        QueryResult result;
        auto qp = plan(q);
        result.stats.candidate_segments = qp.segments.size();

        size_t skipped_matches = 0;

        for (const auto& meta : qp.segments) {
            if (!segment_can_contain(meta, q)) {
                ++result.stats.segments_skipped;
                continue;
            }

            ++result.stats.segments_scanned;

            SegmentReadConfig rc;
            rc.verify_crc = q.verify_crc;
            rc.allow_corrupt = q.allow_corrupt;

            SegmentReader reader(meta, rc);
            auto events = reader.read_range(q.start_ts, q.end_ts);

            if (q.order == QueryOrder::Desc) {
                std::reverse(events.begin(), events.end());
            }

            for (auto& evt : events) {
                ++result.stats.events_scanned;

                if (!passes(evt, q)) continue;

                ++result.stats.events_matched;
                update_stats(result.stats, evt);

                if (skipped_matches < q.offset) {
                    ++skipped_matches;
                    continue;
                }

                if (q.mode == QueryMode::Materialize) {
                    result.events.push_back(std::move(evt));
                }

                ++result.stats.events_returned;

                if (q.limit > 0 && result.stats.events_returned >= q.limit)
                    return result;
            }
        }

        if (q.order == QueryOrder::Asc) {
            std::stable_sort(result.events.begin(), result.events.end(),
                [](const MarketEvent& a, const MarketEvent& b) {
                    return event_timestamp(a) < event_timestamp(b);
                });
        } else {
            std::stable_sort(result.events.begin(), result.events.end(),
                [](const MarketEvent& a, const MarketEvent& b) {
                    return event_timestamp(a) > event_timestamp(b);
                });
        }

        return result;
    }

    QueryStats stream(const TickQuery& q, TickVisitor visitor) const {
        QueryStats stats;
        auto qp = plan(q);
        stats.candidate_segments = qp.segments.size();

        size_t skipped_matches = 0;

        for (const auto& meta : qp.segments) {
            if (!segment_can_contain(meta, q)) {
                ++stats.segments_skipped;
                continue;
            }

            ++stats.segments_scanned;

            SegmentReadConfig rc;
            rc.verify_crc = q.verify_crc;
            rc.allow_corrupt = q.allow_corrupt;

            SegmentReader reader(meta, rc);
            auto events = reader.read_range(q.start_ts, q.end_ts);

            if (q.order == QueryOrder::Desc) {
                std::reverse(events.begin(), events.end());
            }

            for (const auto& evt : events) {
                ++stats.events_scanned;

                if (!passes(evt, q)) continue;

                ++stats.events_matched;
                update_stats(stats, evt);

                if (skipped_matches < q.offset) {
                    ++skipped_matches;
                    continue;
                }

                ++stats.events_returned;

                if (visitor && !visitor(evt))
                    return stats;

                if (q.limit > 0 && stats.events_returned >= q.limit)
                    return stats;
            }
        }

        return stats;
    }

    size_t count(const TickQuery& q) const {
        TickQuery cq = q;
        cq.limit = 0;
        cq.offset = 0;
        return static_cast<size_t>(stream(cq, [](const MarketEvent&) { return true; }).events_matched);
    }

private:
    const MetadataCatalog& catalog_;

    static bool segment_can_contain(const SegmentMetadata& m, const TickQuery& q) {
        if (!m.overlaps(q.start_ts, q.end_ts)) return false;

        uint64_t type_count = 0;
        if (q.include_l2) type_count += m.l2_count;
        if (q.include_l3) type_count += m.l3_count;
        if (q.include_trades) type_count += m.trade_count;

        return type_count > 0;
    }

    static bool passes(const MarketEvent& evt, const TickQuery& q) {
        int64_t ts = event_timestamp(evt);
        if (ts < q.start_ts || ts > q.end_ts) return false;

        return std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;

            if (!q.symbol.empty() && e.symbol != q.symbol) return false;

            if constexpr (std::is_same_v<T, L2Update>) {
                if (!q.include_l2) return false;
                if (q.side == TickSideFilter::Bid && e.side != BookSide::Bid) return false;
                if (q.side == TickSideFilter::Ask && e.side != BookSide::Ask) return false;
                if (q.side == TickSideFilter::Buy || q.side == TickSideFilter::Sell) return false;
                return price_qty_ok(e.price, e.qty, q);
            }

            if constexpr (std::is_same_v<T, L3Update>) {
                if (!q.include_l3) return false;
                if (q.side == TickSideFilter::Buy && e.side != Side::Buy) return false;
                if (q.side == TickSideFilter::Sell && e.side != Side::Sell) return false;
                if (q.side == TickSideFilter::Bid || q.side == TickSideFilter::Ask) return false;
                return price_qty_ok(e.price, e.qty, q);
            }

            if constexpr (std::is_same_v<T, Trade>) {
                if (!q.include_trades) return false;
                if (q.side == TickSideFilter::Buy && e.aggressor != Side::Buy) return false;
                if (q.side == TickSideFilter::Sell && e.aggressor != Side::Sell) return false;
                if (q.side == TickSideFilter::Bid || q.side == TickSideFilter::Ask) return false;
                return price_qty_ok(e.price, e.qty, q);
            }

            return false;
        }, evt);
    }

    static bool price_qty_ok(double price, double qty, const TickQuery& q) {
        if (!std::isfinite(price) || !std::isfinite(qty)) return false;
        if (price < q.min_price || price > q.max_price) return false;
        if (qty < q.min_qty || qty > q.max_qty) return false;
        return true;
    }

    static void update_stats(QueryStats& s, const MarketEvent& evt) {
        int64_t ts = event_timestamp(evt);

        if (s.events_matched == 1) {
            s.first_ts = ts;
            s.last_ts = ts;
        } else {
            s.first_ts = std::min(s.first_ts, ts);
            s.last_ts = std::max(s.last_ts, ts);
        }

        std::visit([&](const auto& e) {
            double px = e.price;
            double qty = e.qty;

            s.min_price = std::min(s.min_price, px);
            s.max_price = std::max(s.max_price, px);
            s.total_qty += qty;
            s.total_notional += px * qty;
        }, evt);
    }
};

} // namespace hft::tickdb