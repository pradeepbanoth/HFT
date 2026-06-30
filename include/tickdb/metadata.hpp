#pragma once
// tickdb/metadata.hpp — advanced segment metadata catalog for institutional tick DB

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft::tickdb {

enum class SegmentStatus : uint8_t {
    Open,
    Closed,
    Verified,
    Corrupt,
    Archived
};

inline const char* segment_status_to_str(SegmentStatus s) noexcept {
    switch (s) {
        case SegmentStatus::Open: return "open";
        case SegmentStatus::Closed: return "closed";
        case SegmentStatus::Verified: return "verified";
        case SegmentStatus::Corrupt: return "corrupt";
        case SegmentStatus::Archived: return "archived";
        default: return "unknown";
    }
}

inline SegmentStatus segment_status_from_str(const std::string& s) {
    if (s == "open") return SegmentStatus::Open;
    if (s == "closed") return SegmentStatus::Closed;
    if (s == "verified") return SegmentStatus::Verified;
    if (s == "corrupt") return SegmentStatus::Corrupt;
    if (s == "archived") return SegmentStatus::Archived;
    return SegmentStatus::Corrupt;
}

struct SegmentMetadata {
    uint64_t id = 0;

    std::string exchange;
    std::string symbol;
    std::string venue_symbol;
    std::string path;

    int64_t start_ts = 0;
    int64_t end_ts = 0;

    uint64_t record_count = 0;
    uint64_t l2_count = 0;
    uint64_t l3_count = 0;
    uint64_t trade_count = 0;

    uint64_t compressed_size = 0;
    uint64_t uncompressed_size = 0;

    uint32_t crc32 = 0;
    uint32_t schema_version = 2;

    bool indexed = true;
    bool encrypted = false;
    bool compressed = false;

    SegmentStatus status = SegmentStatus::Closed;

    double compression_ratio() const noexcept {
        if (uncompressed_size == 0) return 1.0;
        return static_cast<double>(compressed_size) /
               static_cast<double>(uncompressed_size);
    }

    bool valid_time_range() const noexcept {
        return start_ts <= end_ts;
    }

    bool overlaps(int64_t q_start, int64_t q_end) const noexcept {
        return !(end_ts < q_start || start_ts > q_end);
    }

    bool contains(int64_t ts) const noexcept {
        return ts >= start_ts && ts <= end_ts;
    }

    bool is_usable(bool include_corrupt, bool include_archived) const noexcept {
        if (status == SegmentStatus::Corrupt && !include_corrupt) return false;
        if (status == SegmentStatus::Archived && !include_archived) return false;
        return true;
    }
};

struct SegmentQuery {
    std::string exchange;
    std::string symbol;

    int64_t start_ts = std::numeric_limits<int64_t>::min();
    int64_t end_ts = std::numeric_limits<int64_t>::max();

    bool include_open = true;
    bool include_closed = true;
    bool include_verified = true;
    bool include_corrupt = false;
    bool include_archived = false;

    bool require_indexed = false;
};

struct CoverageGap {
    std::string exchange;
    std::string symbol;
    int64_t gap_start = 0;
    int64_t gap_end = 0;

    int64_t duration_ns() const noexcept {
        return std::max<int64_t>(0, gap_end - gap_start);
    }
};

struct CatalogSummary {
    uint64_t segments = 0;
    uint64_t records = 0;
    uint64_t l2 = 0;
    uint64_t l3 = 0;
    uint64_t trades = 0;

    uint64_t corrupt = 0;
    uint64_t archived = 0;
    uint64_t open = 0;
    uint64_t verified = 0;

    uint64_t compressed_bytes = 0;
    uint64_t uncompressed_bytes = 0;

    int64_t first_ts = 0;
    int64_t last_ts = 0;

    double compression_ratio() const noexcept {
        if (uncompressed_bytes == 0) return 1.0;
        return static_cast<double>(compressed_bytes) /
               static_cast<double>(uncompressed_bytes);
    }
};

class MetadataCatalog {
public:
    bool add(SegmentMetadata meta) {
        if (!validate(meta)) return false;

        if (meta.id == 0) meta.id = next_id_++;
        if (segments_.count(meta.id)) return false;

        next_id_ = std::max(next_id_, meta.id + 1);

        uint64_t id = meta.id;
        segments_[id] = std::move(meta);

        rebuild_indexes();
        dirty_ = true;
        return true;
    }

    bool upsert(SegmentMetadata meta) {
        if (!validate(meta)) return false;
        if (meta.id == 0) meta.id = next_id_++;

        next_id_ = std::max(next_id_, meta.id + 1);
        segments_[meta.id] = std::move(meta);

        rebuild_indexes();
        dirty_ = true;
        return true;
    }

    bool update(const SegmentMetadata& meta) {
        if (!validate(meta)) return false;

        auto it = segments_.find(meta.id);
        if (it == segments_.end()) return false;

        it->second = meta;
        rebuild_indexes();
        dirty_ = true;
        return true;
    }

    bool update_status(uint64_t id, SegmentStatus status) {
        auto it = segments_.find(id);
        if (it == segments_.end()) return false;

        it->second.status = status;
        dirty_ = true;
        return true;
    }

    bool remove(uint64_t id) {
        auto n = segments_.erase(id);
        if (n == 0) return false;

        rebuild_indexes();
        dirty_ = true;
        return true;
    }

    std::optional<SegmentMetadata> get(uint64_t id) const {
        auto it = segments_.find(id);
        if (it == segments_.end()) return std::nullopt;
        return it->second;
    }

    std::vector<SegmentMetadata> query(const SegmentQuery& q) const {
        std::vector<SegmentMetadata> out;

        const auto candidate_ids = candidate_ids_for(q);

        for (uint64_t id : candidate_ids) {
            auto it = segments_.find(id);
            if (it == segments_.end()) continue;

            const auto& m = it->second;

            if (!q.exchange.empty() && m.exchange != q.exchange) continue;
            if (!q.symbol.empty() && m.symbol != q.symbol) continue;
            if (!m.overlaps(q.start_ts, q.end_ts)) continue;
            if (q.require_indexed && !m.indexed) continue;
            if (!status_allowed(m.status, q)) continue;

            out.push_back(m);
        }

        sort_segments(out);
        return out;
    }

    std::vector<SegmentMetadata> query_symbol(
        const std::string& exchange,
        const std::string& symbol,
        int64_t start_ts,
        int64_t end_ts
    ) const {
        SegmentQuery q;
        q.exchange = exchange;
        q.symbol = symbol;
        q.start_ts = start_ts;
        q.end_ts = end_ts;
        return query(q);
    }

    std::vector<std::string> symbols() const {
        std::vector<std::string> out;
        out.reserve(symbol_index_.size());

        for (const auto& [sym, _] : symbol_index_)
            out.push_back(sym);

        std::sort(out.begin(), out.end());
        return out;
    }

    std::vector<std::string> exchanges() const {
        std::vector<std::string> out;
        out.reserve(exchange_index_.size());

        for (const auto& [ex, _] : exchange_index_)
            out.push_back(ex);

        std::sort(out.begin(), out.end());
        return out;
    }

    CatalogSummary summary() const {
        CatalogSummary s;
        s.segments = segments_.size();

        bool first = true;

        for (const auto& [id, m] : segments_) {
            s.records += m.record_count;
            s.l2 += m.l2_count;
            s.l3 += m.l3_count;
            s.trades += m.trade_count;
            s.compressed_bytes += m.compressed_size;
            s.uncompressed_bytes += m.uncompressed_size;

            if (m.status == SegmentStatus::Corrupt) ++s.corrupt;
            if (m.status == SegmentStatus::Archived) ++s.archived;
            if (m.status == SegmentStatus::Open) ++s.open;
            if (m.status == SegmentStatus::Verified) ++s.verified;

            if (first) {
                s.first_ts = m.start_ts;
                s.last_ts = m.end_ts;
                first = false;
            } else {
                s.first_ts = std::min(s.first_ts, m.start_ts);
                s.last_ts = std::max(s.last_ts, m.end_ts);
            }
        }

        return s;
    }

    std::vector<CoverageGap> find_gaps(
        const std::string& exchange,
        const std::string& symbol,
        int64_t start_ts,
        int64_t end_ts,
        int64_t max_allowed_gap_ns = 0
    ) const {
        std::vector<CoverageGap> gaps;

        auto segs = query_symbol(exchange, symbol, start_ts, end_ts);
        if (segs.empty()) {
            gaps.push_back({exchange, symbol, start_ts, end_ts});
            return gaps;
        }

        int64_t cursor = start_ts;

        for (const auto& s : segs) {
            if (s.start_ts > cursor + max_allowed_gap_ns) {
                gaps.push_back({exchange, symbol, cursor, s.start_ts});
            }
            cursor = std::max(cursor, s.end_ts);
        }

        if (cursor < end_ts - max_allowed_gap_ns) {
            gaps.push_back({exchange, symbol, cursor, end_ts});
        }

        return gaps;
    }

    std::vector<std::pair<uint64_t, uint64_t>> find_overlaps() const {
        std::vector<std::pair<uint64_t, uint64_t>> overlaps;
        auto all = all_segments_sorted();

        for (size_t i = 0; i < all.size(); ++i) {
            for (size_t j = i + 1; j < all.size(); ++j) {
                if (all[i].exchange != all[j].exchange ||
                    all[i].symbol != all[j].symbol) {
                    continue;
                }

                if (all[j].start_ts > all[i].end_ts) break;

                if (all[i].overlaps(all[j].start_ts, all[j].end_ts)) {
                    overlaps.push_back({all[i].id, all[j].id});
                }
            }
        }

        return overlaps;
    }

    uint64_t size() const noexcept {
        return segments_.size();
    }

    bool empty() const noexcept {
        return segments_.empty();
    }

    bool dirty() const noexcept {
        return dirty_;
    }

    void clear_dirty() noexcept {
        dirty_ = false;
    }

    void clear() {
        segments_.clear();
        symbol_index_.clear();
        exchange_index_.clear();
        next_id_ = 1;
        dirty_ = true;
    }

    bool save_csv(const std::string& path, bool atomic = true) const {
        const std::string tmp = atomic ? path + ".tmp" : path;

        std::ofstream out(tmp);
        if (!out) return false;

        out << "manifest_version,2\n";
        out << "id,exchange,symbol,venue_symbol,path,start_ts,end_ts,record_count,l2_count,l3_count,"
               "trade_count,compressed_size,uncompressed_size,crc32,schema_version,indexed,"
               "encrypted,compressed,status\n";

        auto all = all_segments_sorted();

        for (const auto& m : all) {
            out << m.id << ","
                << escape(m.exchange) << ","
                << escape(m.symbol) << ","
                << escape(m.venue_symbol) << ","
                << escape(m.path) << ","
                << m.start_ts << ","
                << m.end_ts << ","
                << m.record_count << ","
                << m.l2_count << ","
                << m.l3_count << ","
                << m.trade_count << ","
                << m.compressed_size << ","
                << m.uncompressed_size << ","
                << m.crc32 << ","
                << m.schema_version << ","
                << bool_to_str(m.indexed) << ","
                << bool_to_str(m.encrypted) << ","
                << bool_to_str(m.compressed) << ","
                << segment_status_to_str(m.status)
                << "\n";
        }

        out.close();

        if (atomic) {
            std::remove(path.c_str());
            if (std::rename(tmp.c_str(), path.c_str()) != 0)
                return false;
        }

        return true;
    }

    bool load_csv(const std::string& path) {
        std::ifstream in(path);
        if (!in) return false;

        segments_.clear();
        next_id_ = 1;

        std::string line;
        if (!std::getline(in, line)) return false;

        bool v2 = line.rfind("manifest_version,2", 0) == 0;

        if (v2) {
            std::getline(in, line); // header
        }

        while (std::getline(in, line)) {
            if (line.empty()) continue;

            auto cols = split_csv(line);

            SegmentMetadata m;

            if (v2) {
                if (cols.size() < 19) continue;

                m.id = std::stoull(cols[0]);
                m.exchange = cols[1];
                m.symbol = cols[2];
                m.venue_symbol = cols[3];
                m.path = cols[4];
                m.start_ts = std::stoll(cols[5]);
                m.end_ts = std::stoll(cols[6]);
                m.record_count = std::stoull(cols[7]);
                m.l2_count = std::stoull(cols[8]);
                m.l3_count = std::stoull(cols[9]);
                m.trade_count = std::stoull(cols[10]);
                m.compressed_size = std::stoull(cols[11]);
                m.uncompressed_size = std::stoull(cols[12]);
                m.crc32 = static_cast<uint32_t>(std::stoul(cols[13]));
                m.schema_version = static_cast<uint32_t>(std::stoul(cols[14]));
                m.indexed = str_to_bool(cols[15]);
                m.encrypted = str_to_bool(cols[16]);
                m.compressed = str_to_bool(cols[17]);
                m.status = segment_status_from_str(cols[18]);
            } else {
                if (cols.size() < 14) continue;

                m.id = std::stoull(cols[0]);
                m.exchange = cols[1];
                m.symbol = cols[2];
                m.path = cols[3];
                m.start_ts = std::stoll(cols[4]);
                m.end_ts = std::stoll(cols[5]);
                m.record_count = std::stoull(cols[6]);
                m.l2_count = std::stoull(cols[7]);
                m.l3_count = std::stoull(cols[8]);
                m.trade_count = std::stoull(cols[9]);
                m.compressed_size = std::stoull(cols[10]);
                m.uncompressed_size = std::stoull(cols[11]);
                m.crc32 = static_cast<uint32_t>(std::stoul(cols[12]));
                m.status = segment_status_from_str(cols[13]);
            }

            if (validate(m)) {
                segments_[m.id] = std::move(m);
                next_id_ = std::max(next_id_, m.id + 1);
            }
        }

        rebuild_indexes();
        dirty_ = false;
        return true;
    }

private:
    std::unordered_map<uint64_t, SegmentMetadata> segments_;
    std::unordered_map<std::string, std::set<uint64_t>> symbol_index_;
    std::unordered_map<std::string, std::set<uint64_t>> exchange_index_;
    uint64_t next_id_ = 1;
    bool dirty_ = false;

    static bool validate(const SegmentMetadata& m) {
        if (m.exchange.empty()) return false;
        if (m.symbol.empty()) return false;
        if (m.path.empty()) return false;
        if (!m.valid_time_range()) return false;
        if (m.record_count < m.l2_count + m.l3_count + m.trade_count) return false;
        return true;
    }

    static bool status_allowed(SegmentStatus s, const SegmentQuery& q) {
        if (s == SegmentStatus::Open) return q.include_open;
        if (s == SegmentStatus::Closed) return q.include_closed;
        if (s == SegmentStatus::Verified) return q.include_verified;
        if (s == SegmentStatus::Corrupt) return q.include_corrupt;
        if (s == SegmentStatus::Archived) return q.include_archived;
        return false;
    }

    void rebuild_indexes() {
        symbol_index_.clear();
        exchange_index_.clear();

        for (const auto& [id, m] : segments_) {
            symbol_index_[m.symbol].insert(id);
            exchange_index_[m.exchange].insert(id);
        }
    }

    std::vector<uint64_t> candidate_ids_for(const SegmentQuery& q) const {
        std::set<uint64_t> result;

        if (!q.symbol.empty()) {
            auto it = symbol_index_.find(q.symbol);
            if (it == symbol_index_.end()) return {};
            result = it->second;
        } else {
            for (const auto& [id, _] : segments_)
                result.insert(id);
        }

        if (!q.exchange.empty()) {
            auto eit = exchange_index_.find(q.exchange);
            if (eit == exchange_index_.end()) return {};

            std::set<uint64_t> intersection;
            std::set_intersection(
                result.begin(), result.end(),
                eit->second.begin(), eit->second.end(),
                std::inserter(intersection, intersection.begin())
            );

            result = std::move(intersection);
        }

        return {result.begin(), result.end()};
    }

    std::vector<SegmentMetadata> all_segments_sorted() const {
        std::vector<SegmentMetadata> out;
        out.reserve(segments_.size());

        for (const auto& [id, m] : segments_)
            out.push_back(m);

        sort_segments(out);
        return out;
    }

    static void sort_segments(std::vector<SegmentMetadata>& out) {
        std::sort(out.begin(), out.end(),
            [](const SegmentMetadata& a, const SegmentMetadata& b) {
                if (a.exchange != b.exchange) return a.exchange < b.exchange;
                if (a.symbol != b.symbol) return a.symbol < b.symbol;
                if (a.start_ts != b.start_ts) return a.start_ts < b.start_ts;
                return a.id < b.id;
            });
    }

    static const char* bool_to_str(bool v) noexcept {
        return v ? "1" : "0";
    }

    static bool str_to_bool(const std::string& s) {
        return s == "1" || s == "true" || s == "yes";
    }

    static std::string escape(const std::string& s) {
        bool needs_quotes =
            s.find(',') != std::string::npos ||
            s.find('"') != std::string::npos ||
            s.find('\n') != std::string::npos;

        if (!needs_quotes) return s;

        std::string out = "\"";
        for (char c : s) {
            if (c == '"') out += "\"\"";
            else out += c;
        }
        out += "\"";
        return out;
    }

    static std::vector<std::string> split_csv(const std::string& line) {
        std::vector<std::string> cols;
        std::string cur;
        bool quoted = false;

        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];

            if (quoted) {
                if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                    cur += '"';
                    ++i;
                } else if (c == '"') {
                    quoted = false;
                } else {
                    cur += c;
                }
            } else {
                if (c == '"') {
                    quoted = true;
                } else if (c == ',') {
                    cols.push_back(cur);
                    cur.clear();
                } else {
                    cur += c;
                }
            }
        }

        cols.push_back(cur);
        return cols;
    }
};

} // namespace hft::tickdb