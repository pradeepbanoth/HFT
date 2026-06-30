#pragma once
// tickdb/segment.hpp — advanced segment writer/reader + rolling segment manager

#include "tick_store.hpp"
#include "tickdb/metadata.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace hft::tickdb {

struct SegmentWriteConfig {
    std::string exchange = "unknown";
    std::string symbol;
    std::string venue_symbol;
    std::string root_dir = "data/tickdb";
    std::string path;

    bool sort_by_ts = true;
    bool verify_after_write = true;
    bool atomic_write = true;

    uint64_t max_records_per_segment = 1'000'000;
};

struct SegmentReadConfig {
    bool verify_crc = true;
    bool allow_corrupt = false;
};

class SegmentPathBuilder {
public:
    static std::string build(
        const std::string& root,
        const std::string& exchange,
        const std::string& symbol,
        int64_t start_ts,
        uint64_t segment_id
    ) {
        std::filesystem::path p(root);
        p /= exchange;
        p /= symbol;
        p /= std::to_string(start_ts / 1'000'000'000LL);
        p /= "segment_" + pad_id(segment_id) + ".tickdb";
        return p.string();
    }

private:
    static std::string pad_id(uint64_t id) {
        std::string s = std::to_string(id);
        if (s.size() >= 8) return s;
        return std::string(8 - s.size(), '0') + s;
    }
};

class SegmentValidator {
public:
    static bool validate_file(const SegmentMetadata& meta, bool verify_crc = true) {
        try {
            TickStoreReader reader(meta.path);
            TickReplayFilter f;
            f.symbol = meta.symbol;
            f.start_ts = meta.start_ts;
            f.end_ts = meta.end_ts;
            f.verify_crc = verify_crc;

            auto events = reader.read_range(f);
            return events.size() == meta.record_count;
        } catch (...) {
            return false;
        }
    }

    static std::vector<std::string> validate_metadata(const SegmentMetadata& m) {
        std::vector<std::string> errors;

        if (m.exchange.empty()) errors.push_back("exchange_empty");
        if (m.symbol.empty()) errors.push_back("symbol_empty");
        if (m.path.empty()) errors.push_back("path_empty");
        if (m.start_ts > m.end_ts) errors.push_back("invalid_time_range");
        if (m.record_count < m.l2_count + m.l3_count + m.trade_count)
            errors.push_back("record_count_less_than_components");

        if (m.uncompressed_size == 0) errors.push_back("empty_file_size");

        return errors;
    }
};

class SegmentWriter {
public:
    explicit SegmentWriter(SegmentWriteConfig cfg)
        : cfg_(std::move(cfg))
    {
        if (cfg_.symbol.empty()) throw std::runtime_error("SegmentWriter: symbol missing");

        if (cfg_.venue_symbol.empty())
            cfg_.venue_symbol = cfg_.symbol;
    }

    SegmentMetadata write(const std::vector<MarketEvent>& events, uint64_t segment_id = 0) {
        if (events.empty())
            throw std::runtime_error("SegmentWriter: no events to write");

        int64_t first_ts = min_ts(events);

        std::string final_path = cfg_.path.empty()
            ? SegmentPathBuilder::build(cfg_.root_dir, cfg_.exchange, cfg_.symbol, first_ts, segment_id)
            : cfg_.path;

        std::filesystem::create_directories(std::filesystem::path(final_path).parent_path());

        std::string write_path = cfg_.atomic_write ? final_path + ".tmp" : final_path;

        {
            TickStoreWriter writer(write_path);
            writer.write_many(events, cfg_.sort_by_ts);
            writer.finalize();
        }

        if (cfg_.atomic_write) {
            std::remove(final_path.c_str());
            if (std::rename(write_path.c_str(), final_path.c_str()) != 0) {
                throw std::runtime_error("SegmentWriter: atomic rename failed");
            }
        }

        auto stats = TickStore::inspect(final_path);

        SegmentMetadata meta;
        meta.id = segment_id;
        meta.exchange = cfg_.exchange;
        meta.symbol = cfg_.symbol;
        meta.venue_symbol = cfg_.venue_symbol;
        meta.path = final_path;
        meta.start_ts = stats.first_ts;
        meta.end_ts = stats.last_ts;
        meta.record_count = stats.records;
        meta.l2_count = stats.l2;
        meta.l3_count = stats.l3;
        meta.trade_count = stats.trades;
        meta.uncompressed_size = file_size(final_path);
        meta.compressed_size = meta.uncompressed_size;
        meta.schema_version = 2;
        meta.indexed = true;
        meta.encrypted = false;
        meta.compressed = false;
        meta.status = SegmentStatus::Closed;

        auto metadata_errors = SegmentValidator::validate_metadata(meta);
        if (!metadata_errors.empty()) {
            meta.status = SegmentStatus::Corrupt;
            return meta;
        }

        if (cfg_.verify_after_write) {
            meta.status = SegmentValidator::validate_file(meta, true)
                ? SegmentStatus::Verified
                : SegmentStatus::Corrupt;
        }

        return meta;
    }

private:
    SegmentWriteConfig cfg_;

    static int64_t min_ts(const std::vector<MarketEvent>& events) {
        int64_t m = event_timestamp(events.front());
        for (const auto& e : events)
            m = std::min(m, event_timestamp(e));
        return m;
    }

    static uint64_t file_size(const std::string& path) {
        std::error_code ec;
        auto n = std::filesystem::file_size(path, ec);
        return ec ? 0 : static_cast<uint64_t>(n);
    }
};

class SegmentReader {
public:
    SegmentReader(SegmentMetadata meta, SegmentReadConfig cfg = {})
        : meta_(std::move(meta))
        , cfg_(cfg)
        , reader_(meta_.path)
    {
        if (meta_.status == SegmentStatus::Corrupt && !cfg_.allow_corrupt)
            throw std::runtime_error("SegmentReader: refusing corrupt segment");
    }

    const SegmentMetadata& metadata() const noexcept {
        return meta_;
    }

    std::vector<MarketEvent> read_all() {
        return read_range(meta_.start_ts, meta_.end_ts);
    }

    std::vector<MarketEvent> read_range(int64_t start_ts, int64_t end_ts) {
        TickReplayFilter f;
        f.symbol = meta_.symbol;
        f.start_ts = std::max(start_ts, meta_.start_ts);
        f.end_ts = std::min(end_ts, meta_.end_ts);
        f.verify_crc = cfg_.verify_crc;
        return reader_.read_range(f);
    }

    bool verify() {
        return SegmentValidator::validate_file(meta_, cfg_.verify_crc);
    }

private:
    SegmentMetadata meta_;
    SegmentReadConfig cfg_;
    TickStoreReader reader_;
};

class SegmentCatalogWriter {
public:
    explicit SegmentCatalogWriter(MetadataCatalog& catalog)
        : catalog_(catalog) {}

    SegmentMetadata write_and_register(
        const SegmentWriteConfig& cfg,
        const std::vector<MarketEvent>& events,
        uint64_t segment_id = 0
    ) {
        SegmentWriter writer(cfg);
        auto meta = writer.write(events, segment_id);
        catalog_.upsert(meta);
        return meta;
    }

private:
    MetadataCatalog& catalog_;
};

class RollingSegmentWriter {
public:
    RollingSegmentWriter(MetadataCatalog& catalog, SegmentWriteConfig cfg)
        : catalog_(catalog)
        , cfg_(std::move(cfg))
    {
        if (cfg_.max_records_per_segment == 0)
            cfg_.max_records_per_segment = 1'000'000;
    }

    void append(MarketEvent evt) {
        buffer_.push_back(std::move(evt));
        if (buffer_.size() >= cfg_.max_records_per_segment)
            flush();
    }

    void append_many(const std::vector<MarketEvent>& events) {
        for (const auto& e : events)
            append(e);
    }

    std::optional<SegmentMetadata> flush() {
        if (buffer_.empty()) return std::nullopt;

        uint64_t id = next_segment_id_++;

        SegmentWriter writer(cfg_);
        auto meta = writer.write(buffer_, id);
        catalog_.upsert(meta);

        buffer_.clear();
        return meta;
    }

    size_t buffered() const noexcept {
        return buffer_.size();
    }

private:
    MetadataCatalog& catalog_;
    SegmentWriteConfig cfg_;
    std::vector<MarketEvent> buffer_;
    uint64_t next_segment_id_ = 1;
};

class SegmentQueryReader {
public:
    explicit SegmentQueryReader(const MetadataCatalog& catalog)
        : catalog_(catalog) {}

    std::vector<MarketEvent> read(const SegmentQuery& q, SegmentReadConfig read_cfg = {}) {
        std::vector<MarketEvent> out;

        auto segments = catalog_.query(q);
        for (const auto& meta : segments) {
            SegmentReader reader(meta, read_cfg);
            auto events = reader.read_range(q.start_ts, q.end_ts);
            out.insert(out.end(), events.begin(), events.end());
        }

        std::stable_sort(out.begin(), out.end(),
            [](const MarketEvent& a, const MarketEvent& b) {
                return event_timestamp(a) < event_timestamp(b);
            });

        return out;
    }

private:
    const MetadataCatalog& catalog_;
};

} // namespace hft::tickdb