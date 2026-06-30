#pragma once
// tick_store.hpp — advanced deterministic tick database with embedded index + CRC

#include "types.hpp"
#include "simulator.hpp"
#include "event_source.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

enum class TickKind : uint8_t { L2 = 1, L3 = 2, Trade = 3 };

struct TickStoreHeader {
    char magic[8] = {'H','F','T','T','D','B','2','\0'};
    uint32_t version = 2;
    uint32_t flags = 0;
    uint64_t index_offset = 0;
    uint64_t record_count = 0;
    int64_t first_ts = 0;
    int64_t last_ts = 0;
    int64_t created_ns = 0;
};

struct TickRecordHeader {
    TickKind kind = TickKind::L2;
    uint32_t payload_size = 0;
    int64_t ts = 0;
    uint64_t seq = 0;
    uint32_t crc32 = 0;
};

struct TickIndexEntry {
    int64_t ts = 0;
    uint64_t offset = 0;
    uint64_t seq = 0;
    TickKind kind = TickKind::L2;
    char symbol[24]{};
};

struct TickStoreStats {
    uint64_t records = 0;
    uint64_t l2 = 0;
    uint64_t l3 = 0;
    uint64_t trades = 0;
    int64_t first_ts = 0;
    int64_t last_ts = 0;
};

struct TickReplayFilter {
    std::string symbol;
    int64_t start_ts = std::numeric_limits<int64_t>::min();
    int64_t end_ts = std::numeric_limits<int64_t>::max();
    bool include_l2 = true;
    bool include_l3 = true;
    bool include_trades = true;
    bool verify_crc = true;
};

struct PackedTickL2 {
    char symbol[24]{};
    BookSide side = BookSide::Bid;
    double price = 0.0;
    double qty = 0.0;
    int64_t timestamp = 0;
    int64_t seq = 0;
};

struct PackedTickL3 {
    char symbol[24]{};
    char order_id[48]{};
    L3Event event = L3Event::Add;
    Side side = Side::Buy;
    double price = 0.0;
    double qty = 0.0;
    int64_t timestamp = 0;
    int64_t seq = 0;
};

struct PackedTickTrade {
    char trade_id[48]{};
    char symbol[24]{};
    Side side = Side::Buy;
    double price = 0.0;
    double qty = 0.0;
    int64_t timestamp = 0;
    Side aggressor = Side::Buy;
    bool buyer_mm = false;
};

namespace tickdb_detail {

inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

template<size_t N>
inline void assign(char (&dst)[N], const std::string& s) {
    std::fill(dst, dst + N, '\0');
    std::copy_n(s.data(), std::min<size_t>(s.size(), N - 1), dst);
}

inline uint32_t crc32(const uint8_t* data, size_t len) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (0xEDB88320u & ~((crc & 1u) - 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

inline const char* symbol_of(const MarketEvent& evt) {
    return std::visit([](const auto& e) { return e.symbol.c_str(); }, evt);
}

inline TickKind kind_of(const MarketEvent& evt) {
    return std::visit([](const auto& e) -> TickKind {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, L2Update>) return TickKind::L2;
        if constexpr (std::is_same_v<T, L3Update>) return TickKind::L3;
        return TickKind::Trade;
    }, evt);
}

} // namespace tickdb_detail

class TickStoreWriter {
public:
    explicit TickStoreWriter(const std::string& path)
        : path_(path), out_(path, std::ios::binary)
    {
        if (!out_) throw std::runtime_error("TickStoreWriter: cannot open " + path);

        header_.created_ns = tickdb_detail::now_ns();
        out_.write(reinterpret_cast<const char*>(&header_), sizeof(header_));
    }

    ~TickStoreWriter() {
        try { finalize(); } catch (...) {}
    }

    void write(const MarketEvent& evt) {
        std::visit([&](const auto& e) { write_impl(e); }, evt);
    }

    void write_many(const std::vector<MarketEvent>& events, bool sort_by_ts = false) {
        if (!sort_by_ts) {
            for (const auto& e : events) write(e);
            return;
        }

        auto copy = events;
        std::stable_sort(copy.begin(), copy.end(),
            [](const MarketEvent& a, const MarketEvent& b) {
                return event_timestamp(a) < event_timestamp(b);
            });

        for (const auto& e : copy) write(e);
    }

    void finalize() {
        if (finalized_) return;

        header_.index_offset = static_cast<uint64_t>(out_.tellp());

        uint64_t n = index_.size();
        out_.write(reinterpret_cast<const char*>(&n), sizeof(n));
        if (n > 0)
            out_.write(reinterpret_cast<const char*>(index_.data()),
                       static_cast<std::streamsize>(index_.size() * sizeof(TickIndexEntry)));

        header_.record_count = stats_.records;
        header_.first_ts = stats_.first_ts;
        header_.last_ts = stats_.last_ts;

        out_.seekp(0, std::ios::beg);
        out_.write(reinterpret_cast<const char*>(&header_), sizeof(header_));
        out_.flush();

        finalized_ = true;
    }

    const TickStoreStats& stats() const noexcept { return stats_; }
    const std::vector<TickIndexEntry>& index() const noexcept { return index_; }

private:
    std::string path_;
    std::ofstream out_;
    TickStoreHeader header_;
    TickStoreStats stats_;
    std::vector<TickIndexEntry> index_;
    uint64_t seq_ = 0;
    bool finalized_ = false;

    template<typename Packed>
    void write_raw(TickKind kind, const Packed& payload, int64_t ts, const std::string& symbol) {
        uint64_t offset = static_cast<uint64_t>(out_.tellp());

        TickRecordHeader rh;
        rh.kind = kind;
        rh.payload_size = sizeof(Packed);
        rh.ts = ts;
        rh.seq = ++seq_;
        rh.crc32 = tickdb_detail::crc32(
            reinterpret_cast<const uint8_t*>(&payload),
            sizeof(Packed)
        );

        out_.write(reinterpret_cast<const char*>(&rh), sizeof(rh));
        out_.write(reinterpret_cast<const char*>(&payload), sizeof(Packed));

        TickIndexEntry idx;
        idx.ts = ts;
        idx.offset = offset;
        idx.seq = rh.seq;
        idx.kind = kind;
        tickdb_detail::assign(idx.symbol, symbol);
        index_.push_back(idx);

        update_stats(kind, ts);
    }

    void write_impl(const L2Update& e) {
        PackedTickL2 p;
        tickdb_detail::assign(p.symbol, e.symbol);
        p.side = e.side;
        p.price = e.price;
        p.qty = e.qty;
        p.timestamp = e.timestamp;
        p.seq = e.seq;
        write_raw(TickKind::L2, p, e.timestamp, e.symbol);
    }

    void write_impl(const L3Update& e) {
        PackedTickL3 p;
        tickdb_detail::assign(p.symbol, e.symbol);
        tickdb_detail::assign(p.order_id, e.order_id);
        p.event = e.event;
        p.side = e.side;
        p.price = e.price;
        p.qty = e.qty;
        p.timestamp = e.timestamp;
        p.seq = e.seq;
        write_raw(TickKind::L3, p, e.timestamp, e.symbol);
    }

    void write_impl(const Trade& e) {
        PackedTickTrade p;
        tickdb_detail::assign(p.trade_id, e.trade_id);
        tickdb_detail::assign(p.symbol, e.symbol);
        p.side = e.side;
        p.price = e.price;
        p.qty = e.qty;
        p.timestamp = e.timestamp;
        p.aggressor = e.aggressor;
        p.buyer_mm = e.buyer_mm;
        write_raw(TickKind::Trade, p, e.timestamp, e.symbol);
    }

    void update_stats(TickKind kind, int64_t ts) {
        ++stats_.records;
        if (kind == TickKind::L2) ++stats_.l2;
        if (kind == TickKind::L3) ++stats_.l3;
        if (kind == TickKind::Trade) ++stats_.trades;

        if (stats_.records == 1) {
            stats_.first_ts = ts;
            stats_.last_ts = ts;
        } else {
            stats_.first_ts = std::min(stats_.first_ts, ts);
            stats_.last_ts = std::max(stats_.last_ts, ts);
        }
    }
};

class TickStoreReader {
public:
    explicit TickStoreReader(const std::string& path)
        : path_(path), in_(path, std::ios::binary)
    {
        if (!in_) throw std::runtime_error("TickStoreReader: cannot open " + path);

        in_.read(reinterpret_cast<char*>(&header_), sizeof(header_));
        if (!in_ || std::string(header_.magic, header_.magic + 7) != "HFTTDB2")
            throw std::runtime_error("TickStoreReader: invalid file format");

        load_index();
        rewind();
    }

    void rewind() {
        in_.clear();
        in_.seekg(sizeof(TickStoreHeader), std::ios::beg);
    }

    const TickStoreHeader& header() const noexcept { return header_; }
    const std::vector<TickIndexEntry>& index() const noexcept { return index_; }

    TickStoreStats stats() const noexcept {
        TickStoreStats s;
        s.records = header_.record_count;
        s.first_ts = header_.first_ts;
        s.last_ts = header_.last_ts;
        for (const auto& e : index_) {
            if (e.kind == TickKind::L2) ++s.l2;
            if (e.kind == TickKind::L3) ++s.l3;
            if (e.kind == TickKind::Trade) ++s.trades;
        }
        return s;
    }

    bool next(MarketEvent& out, bool verify_crc = true) {
        if (static_cast<uint64_t>(in_.tellg()) >= header_.index_offset) return false;

        TickRecordHeader rh;
        in_.read(reinterpret_cast<char*>(&rh), sizeof(rh));
        if (!in_) return false;

        out = read_event(rh, verify_crc);
        return true;
    }

    std::vector<MarketEvent> read_all(TickReplayFilter filter = {}) {
        std::vector<MarketEvent> out;
        rewind();

        MarketEvent evt;
        while (next(evt, filter.verify_crc)) {
            if (passes(evt, filter))
                out.push_back(evt);
        }

        return out;
    }

    std::vector<MarketEvent> read_range(TickReplayFilter filter) {
        std::vector<MarketEvent> out;

        auto first = std::lower_bound(index_.begin(), index_.end(), filter.start_ts,
            [](const TickIndexEntry& e, int64_t ts) { return e.ts < ts; });

        for (auto it = first; it != index_.end(); ++it) {
            if (it->ts > filter.end_ts) break;
            if (!index_passes(*it, filter)) continue;

            in_.clear();
            in_.seekg(static_cast<std::streamoff>(it->offset), std::ios::beg);

            TickRecordHeader rh;
            in_.read(reinterpret_cast<char*>(&rh), sizeof(rh));
            if (!in_) break;

            MarketEvent evt = read_event(rh, filter.verify_crc);
            if (passes(evt, filter))
                out.push_back(std::move(evt));
        }

        return out;
    }

private:
    std::string path_;
    std::ifstream in_;
    TickStoreHeader header_;
    std::vector<TickIndexEntry> index_;

    void load_index() {
        if (header_.index_offset == 0) return;

        in_.clear();
        in_.seekg(static_cast<std::streamoff>(header_.index_offset), std::ios::beg);

        uint64_t n = 0;
        in_.read(reinterpret_cast<char*>(&n), sizeof(n));
        if (!in_) throw std::runtime_error("TickStoreReader: missing index");

        index_.resize(static_cast<size_t>(n));
        if (n > 0) {
            in_.read(reinterpret_cast<char*>(index_.data()),
                     static_cast<std::streamsize>(n * sizeof(TickIndexEntry)));
            if (!in_) throw std::runtime_error("TickStoreReader: corrupted index");
        }
    }

    MarketEvent read_event(const TickRecordHeader& rh, bool verify_crc) {
        switch (rh.kind) {
            case TickKind::L2: {
                PackedTickL2 p;
                read_payload(p, rh, verify_crc);
                return L2Update{std::string(p.symbol), p.side, p.price, p.qty, p.timestamp, p.seq};
            }
            case TickKind::L3: {
                PackedTickL3 p;
                read_payload(p, rh, verify_crc);
                return L3Update{std::string(p.symbol), p.event, std::string(p.order_id),
                                p.side, p.price, p.qty, p.timestamp, p.seq};
            }
            case TickKind::Trade: {
                PackedTickTrade p;
                read_payload(p, rh, verify_crc);
                return Trade{std::string(p.trade_id), std::string(p.symbol), p.side,
                             p.price, p.qty, p.timestamp, p.aggressor, p.buyer_mm};
            }
            default:
                throw std::runtime_error("TickStoreReader: unknown tick kind");
        }
    }

    template<typename T>
    void read_payload(T& payload, const TickRecordHeader& rh, bool verify_crc) {
        if (rh.payload_size != sizeof(T))
            throw std::runtime_error("TickStoreReader: payload size mismatch");

        in_.read(reinterpret_cast<char*>(&payload), sizeof(T));
        if (!in_) throw std::runtime_error("TickStoreReader: truncated payload");

        if (verify_crc) {
            uint32_t got = tickdb_detail::crc32(
                reinterpret_cast<const uint8_t*>(&payload),
                sizeof(T)
            );
            if (got != rh.crc32)
                throw std::runtime_error("TickStoreReader: CRC mismatch");
        }
    }

    static bool index_passes(const TickIndexEntry& e, const TickReplayFilter& f) {
        if (!f.symbol.empty() && f.symbol != std::string(e.symbol)) return false;
        if (e.kind == TickKind::L2 && !f.include_l2) return false;
        if (e.kind == TickKind::L3 && !f.include_l3) return false;
        if (e.kind == TickKind::Trade && !f.include_trades) return false;
        return true;
    }

    static bool passes(const MarketEvent& evt, const TickReplayFilter& f) {
        int64_t ts = event_timestamp(evt);
        if (ts < f.start_ts || ts > f.end_ts) return false;

        return std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;

            if (!f.symbol.empty() && e.symbol != f.symbol) return false;
            if constexpr (std::is_same_v<T, L2Update>) return f.include_l2;
            if constexpr (std::is_same_v<T, L3Update>) return f.include_l3;
            if constexpr (std::is_same_v<T, Trade>) return f.include_trades;

            return false;
        }, evt);
    }
};

class TickStoreEventSource final : public IEventSource {
public:
    explicit TickStoreEventSource(std::string path, TickReplayFilter filter = {})
        : reader_(path), events_(reader_.read_range(filter))
    {}

    bool next(MarketEvent& out) override {
        if (idx_ >= events_.size()) return false;
        out = events_[idx_++];
        return true;
    }

    void reset() override {
        idx_ = 0;
    }

    std::string name() const override {
        return "TickStoreEventSource";
    }

    size_t size() const noexcept {
        return events_.size();
    }

private:
    TickStoreReader reader_;
    std::vector<MarketEvent> events_;
    size_t idx_ = 0;
};

class TickStore {
public:
    static void write_file(
        const std::string& path,
        const std::vector<MarketEvent>& events,
        bool sort_by_ts = true
    ) {
        TickStoreWriter w(path);
        w.write_many(events, sort_by_ts);
        w.finalize();
    }

    static std::vector<MarketEvent> read_file(
        const std::string& path,
        TickReplayFilter filter = {}
    ) {
        TickStoreReader r(path);
        return r.read_range(filter);
    }

    static TickStoreStats inspect(const std::string& path) {
        TickStoreReader r(path);
        return r.stats();
    }
};

} // namespace hft