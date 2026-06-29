#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// event_source.hpp — Unified event-source abstraction for SimEngine
// ─────────────────────────────────────────────────────────────────────────────

#include "types.hpp"
#include "simulator.hpp"
#include "parsers.hpp"

#include <vector>
#include <queue>
#include <memory>
#include <optional>
#include <string>
#include <algorithm>
#include <stdexcept>

namespace hft {

class IEventSource {
public:
    virtual ~IEventSource() = default;

    virtual bool next(MarketEvent& out) = 0;
    virtual void reset() {}

    virtual std::string name() const {
        return "IEventSource";
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// VectorEventSource
// ─────────────────────────────────────────────────────────────────────────────
class VectorEventSource final : public IEventSource {
public:
    explicit VectorEventSource(std::vector<MarketEvent> events, bool sort_by_ts = true)
        : events_(std::move(events))
    {
        if (sort_by_ts) {
            std::stable_sort(events_.begin(), events_.end(),
                [](const MarketEvent& a, const MarketEvent& b) {
                    return event_timestamp(a) < event_timestamp(b);
                });
        }
    }

    bool next(MarketEvent& out) override {
        if (idx_ >= events_.size()) return false;
        out = events_[idx_++];
        return true;
    }

    void reset() override {
        idx_ = 0;
    }

    std::string name() const override {
        return "VectorEventSource";
    }

    size_t size() const noexcept {
        return events_.size();
    }

private:
    std::vector<MarketEvent> events_;
    size_t idx_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// CsvL2EventSource
// ─────────────────────────────────────────────────────────────────────────────
class CsvL2EventSource final : public IEventSource {
public:
    CsvL2EventSource(
        const std::string& filepath,
        const std::string& symbol,
        const std::string& ts_col = "exchange_timestamp",
        const std::string& side_col = "side",
        const std::string& price_col = "price",
        const std::string& qty_col = "amount"
    )
        : reader_(filepath, symbol, ts_col, side_col, price_col, qty_col)
    {}

    bool next(MarketEvent& out) override {
        L2Update upd;
        if (!reader_.next(upd)) return false;
        out = upd;
        return true;
    }

    std::string name() const override {
        return "CsvL2EventSource";
    }

private:
    CsvL2Reader reader_;
};

// ─────────────────────────────────────────────────────────────────────────────
// CsvTradeEventSource
// ─────────────────────────────────────────────────────────────────────────────
class CsvTradeEventSource final : public IEventSource {
public:
    CsvTradeEventSource(
        const std::string& filepath,
        const std::string& symbol,
        const std::string& ts_col = "exchange_timestamp",
        const std::string& price_col = "price",
        const std::string& qty_col = "amount",
        const std::string& side_col = "side",
        const std::string& id_col = "id"
    )
        : reader_(filepath, symbol, ts_col, price_col, qty_col, side_col, id_col)
    {}

    bool next(MarketEvent& out) override {
        Trade trade;
        if (!reader_.next(trade)) return false;
        out = trade;
        return true;
    }

    std::string name() const override {
        return "CsvTradeEventSource";
    }

private:
    CsvTradeReader reader_;
};

// ─────────────────────────────────────────────────────────────────────────────
// MergedEventSource
// Merges multiple sorted sources by timestamp.
// ─────────────────────────────────────────────────────────────────────────────
class MergedEventSource final : public IEventSource {
public:
    explicit MergedEventSource(std::vector<std::unique_ptr<IEventSource>> sources)
        : sources_(std::move(sources))
    {
        refill_all();
    }

    bool next(MarketEvent& out) override {
        if (heap_.empty()) return false;

        Node node = heap_.top();
        heap_.pop();

        out = std::move(node.event);

        MarketEvent next_evt;
        if (sources_[node.source_idx]->next(next_evt)) {
            heap_.push(Node{
                event_timestamp(next_evt),
                node.source_idx,
                std::move(next_evt)
            });
        }

        return true;
    }

    void reset() override {
        for (auto& src : sources_) src->reset();
        while (!heap_.empty()) heap_.pop();
        refill_all();
    }

    std::string name() const override {
        return "MergedEventSource";
    }

private:
    struct Node {
        int64_t ts = 0;
        size_t source_idx = 0;
        MarketEvent event;

        bool operator>(const Node& other) const noexcept {
            return ts > other.ts;
        }
    };

    std::vector<std::unique_ptr<IEventSource>> sources_;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> heap_;

    void refill_all() {
        for (size_t i = 0; i < sources_.size(); ++i) {
            MarketEvent evt;
            if (sources_[i]->next(evt)) {
                heap_.push(Node{
                    event_timestamp(evt),
                    i,
                    std::move(evt)
                });
            }
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// BufferedEventSource
// Converts any source into a reusable in-memory source.
// Useful for optimization/walk-forward where data is replayed many times.
// ─────────────────────────────────────────────────────────────────────────────
class BufferedEventSource final : public IEventSource {
public:
    explicit BufferedEventSource(IEventSource& source, bool sort_by_ts = true) {
        MarketEvent evt;
        while (source.next(evt)) {
            events_.push_back(evt);
        }

        if (sort_by_ts) {
            std::stable_sort(events_.begin(), events_.end(),
                [](const MarketEvent& a, const MarketEvent& b) {
                    return event_timestamp(a) < event_timestamp(b);
                });
        }
    }

    bool next(MarketEvent& out) override {
        if (idx_ >= events_.size()) return false;
        out = events_[idx_++];
        return true;
    }

    void reset() override {
        idx_ = 0;
    }

    std::string name() const override {
        return "BufferedEventSource";
    }

    const std::vector<MarketEvent>& events() const noexcept {
        return events_;
    }

private:
    std::vector<MarketEvent> events_;
    size_t idx_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Source utilities
// ─────────────────────────────────────────────────────────────────────────────
inline std::vector<MarketEvent> collect_events(IEventSource& source, bool sort_by_ts = true) {
    std::vector<MarketEvent> events;
    MarketEvent evt;

    while (source.next(evt)) {
        events.push_back(evt);
    }

    if (sort_by_ts) {
        std::stable_sort(events.begin(), events.end(),
            [](const MarketEvent& a, const MarketEvent& b) {
                return event_timestamp(a) < event_timestamp(b);
            });
    }

    return events;
}

inline SimStats run_source(SimEngine& engine, IEventSource& source) {
    std::vector<MarketEvent> events = collect_events(source, true);
    return engine.run(events);
}

// ─────────────────────────────────────────────────────────────────────────────
// Manual-drive source runner
// Avoids storing the whole stream in memory.
// Good for CSV/live-style large streams.
// ─────────────────────────────────────────────────────────────────────────────
inline SimStats run_source_streaming(SimEngine& engine, IEventSource& source) {
    engine.on_start_manual();

    MarketEvent evt;
    while (source.next(evt)) {
        engine.process_one(evt);
    }

    return engine.on_end_manual();
}

} // namespace hft