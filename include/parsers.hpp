#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// parsers.hpp  —  Exchange feed parsers (header-only, no external JSON lib)
//
//  A minimal, zero-dependency JSON extractor handles the specific message
//  shapes from Binance and Bybit.  For production use, replace the
//  json_get_* helpers with RapidJSON or simdjson for maximum throughput.
//
//  Supported:
//    BinanceParser : @depth (L2 diff), @aggTrade, @trade, @bookTicker
//                   Proper sequence-gap recovery (official Binance protocol)
//    BybitParser   : orderbook.{depth}.{symbol} snapshot+delta, publicTrade
//                   Sequence validation + CRC32 checksum pass-through
//
//  Historical data:
//    CsvL2Reader   : streams L2Update from CSV  (Tardis.dev format)
//    CsvTradeReader: streams Trade   from CSV  (Tardis.dev format)
// ─────────────────────────────────────────────────────────────────────────────

#include "types.hpp"
#include "orderbook.hpp"
#include <string>
#include <vector>
#include <optional>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <unordered_map>
#include <functional>
#include <cstring>
#include <charconv>
#include <cassert>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// Minimal JSON helpers (string_view-based, no heap allocation per call)
// ─────────────────────────────────────────────────────────────────────────────
namespace detail {

// Find the value string for a given key in a flat JSON object.
// Returns empty string_view if not found.
// This is intentionally simple: handles only string/number values at top level.
inline std::string_view json_get(std::string_view json, std::string_view key) {
    // Search for "key":
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return {};
    pos += needle.size();
    // Skip whitespace and colon
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t')) ++pos;
    if (pos >= json.size()) return {};
    // Extract value
    if (json[pos] == '"') {
        // String value
        ++pos;
        auto end = json.find('"', pos);
        if (end == std::string_view::npos) return {};
        return json.substr(pos, end - pos);
    } else {
        // Number/bool/null
        size_t end = pos;
        while (end < json.size() && json[end] != ',' && json[end] != '}' &&
               json[end] != ']' && json[end] != ' ' && json[end] != '\n') ++end;
        return json.substr(pos, end - pos);
    }
}

inline double json_double(std::string_view json, std::string_view key,
                           double def = 0.0) {
    auto v = json_get(json, key);
    if (v.empty()) return def;
    double result = def;
    std::from_chars(v.data(), v.data() + v.size(), result);
    return result;
}

inline int64_t json_int64(std::string_view json, std::string_view key,
                           int64_t def = 0) {
    auto v = json_get(json, key);
    if (v.empty()) return def;
    int64_t result = def;
    std::from_chars(v.data(), v.data() + v.size(), result);
    return result;
}

inline bool json_bool(std::string_view json, std::string_view key,
                       bool def = false) {
    auto v = json_get(json, key);
    if (v.empty()) return def;
    return (v == "true" || v == "1");
}

// Extract array of [price, qty] pairs from a JSON array (Binance depth format)
// Input can be the full outer array e.g. [["43500","1.0"],["43499","2.0"]]
// or after extract_named_array which wraps it: [[["43500","1.0"],...]]
// We look for inner pair brackets [price, qty] by finding '[' followed by a digit or '"'
inline std::vector<std::pair<double,double>>
parse_price_qty_array(std::string_view arr) {
    std::vector<std::pair<double,double>> out;
    size_t pos = 0, len = arr.size();

    while (pos < len) {
        // Find '[' that starts a price-qty pair (preceded possibly by '[' or ',')
        while (pos < len && arr[pos] != '[') ++pos;
        if (pos >= len) break;
        ++pos; // skip '['

        // If next char is another '[', this is the outer array bracket — skip it
        if (pos < len && arr[pos] == '[') { continue; }

        // Skip optional whitespace and opening quote
        while (pos < len && (arr[pos]==' ')) ++pos;
        bool quoted_price = (pos < len && arr[pos] == '"');
        if (quoted_price) ++pos;

        // Parse price
        size_t vstart = pos;
        while (pos < len && arr[pos]!=',' && arr[pos]!='"' && arr[pos]!=']') ++pos;
        double price = 0.0;
        std::from_chars(arr.data()+vstart, arr.data()+pos, price);

        // Skip to qty: past closing quote of price, then comma, then opening quote of qty
        if (pos < len && arr[pos] == '"') ++pos; // closing quote of price
        while (pos < len && (arr[pos]==',' || arr[pos]==' ')) ++pos;
        bool quoted_qty = (pos < len && arr[pos] == '"');
        if (quoted_qty) ++pos;

        // Parse qty
        size_t qstart = pos;
        while (pos < len && arr[pos]!='"' && arr[pos]!=']' && arr[pos]!=',') ++pos;
        double qty = 0.0;
        std::from_chars(arr.data()+qstart, arr.data()+pos, qty);

        if (price > 0.0)
            out.push_back({price, qty});

        // Skip to closing ']' of this pair
        while (pos < len && arr[pos]!=']') ++pos;
        if (pos < len) ++pos;
    }
    return out;
}

// Extract a named array like "b":[[...],[...]] and return its content as string_view
inline std::string_view extract_named_array(std::string_view json,
                                             std::string_view key) {
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return {};
    pos += needle.size();
    while (pos < json.size() && (json[pos]==' '||json[pos]==':'||json[pos]=='\t')) ++pos;
    if (pos >= json.size() || json[pos] != '[') return {};
    // Find matching closing bracket (counting depth)
    size_t start = pos;
    int depth = 0;
    while (pos < json.size()) {
        if (json[pos] == '[') ++depth;
        else if (json[pos] == ']') { if (--depth == 0) { ++pos; break; } }
        ++pos;
    }
    return json.substr(start, pos - start);
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Symbol normalisation
// ─────────────────────────────────────────────────────────────────────────────
inline std::string normalise_symbol(std::string sym) {
    // Remove separators
    sym.erase(std::remove(sym.begin(), sym.end(), '-'), sym.end());
    sym.erase(std::remove(sym.begin(), sym.end(), '/'), sym.end());
    // Uppercase
    for (char& c : sym) c = static_cast<char>(std::toupper(c));
    return sym;
}

// ─────────────────────────────────────────────────────────────────────────────
// Timestamp converters
// ─────────────────────────────────────────────────────────────────────────────
inline int64_t ms_to_ns(int64_t ms) noexcept { return ms * 1'000'000LL; }
inline int64_t us_to_ns(int64_t us) noexcept { return us * 1'000LL; }

// ─────────────────────────────────────────────────────────────────────────────
// BinanceParser
// ─────────────────────────────────────────────────────────────────────────────
class BinanceParser {
public:
    explicit BinanceParser(std::string symbol, bool futures = false)
        : symbol_(std::move(symbol))
        , norm_(normalise_symbol(symbol_))
        , futures_(futures)
    {}

    // ── Initialise from REST depth snapshot ───────────────────────────────────
    // snapshot_json: raw JSON from GET /api/v3/depth?symbol=BTCUSDT&limit=5000
    // ts_ns: capture timestamp in nanoseconds (use current time if 0)
    std::vector<L2Update> apply_snapshot(std::string_view snapshot_json,
                                          int64_t ts_ns = 0)
    {
        if (ts_ns == 0) ts_ns = current_time_ns();

        int64_t last_id = detail::json_int64(snapshot_json, "lastUpdateId");
        std::vector<L2Update> out;

        auto bids_arr = detail::extract_named_array(snapshot_json, "bids");
        auto asks_arr = detail::extract_named_array(snapshot_json, "asks");

        for (auto& [p, q] : detail::parse_price_qty_array(bids_arr))
            out.push_back({norm_, BookSide::Bid, p, q, ts_ns, last_id});
        for (auto& [p, q] : detail::parse_price_qty_array(asks_arr))
            out.push_back({norm_, BookSide::Ask, p, q, ts_ns, last_id});

        last_u_  = last_id;
        synced_  = true;

        // Apply buffered diffs
        for (auto& buffered : pending_diffs_) {
            auto more = parse_depth_diff(buffered);
            out.insert(out.end(), more.begin(), more.end());
        }
        pending_diffs_.clear();
        return out;
    }

    // ── Parse a WebSocket message ─────────────────────────────────────────────
    std::vector<MarketEvent> parse(std::string_view msg_json) {
        // Unwrap combined stream wrapper
        std::string_view data = msg_json;
        auto data_start = msg_json.find("\"data\"");
        if (data_start != std::string_view::npos) {
            auto brace = msg_json.find('{', data_start + 6);
            if (brace != std::string_view::npos) data = msg_json.substr(brace);
        }

        auto etype = detail::json_get(data, "e");
        std::vector<MarketEvent> out;

        if (etype == "depthUpdate") {
            for (auto& e : parse_depth_diff(std::string(data)))
                out.push_back(e);
        } else if (etype == "aggTrade") {
            out.push_back(parse_agg_trade(data));
        } else if (etype == "trade") {
            out.push_back(parse_trade(data));
        } else if (etype == "bookTicker") {
            auto ticks = parse_book_ticker(data);
            for (auto& e : ticks) out.push_back(e);
        }
        return out;
    }

    int64_t gap_count()  const noexcept { return gap_count_; }
    bool    needs_resync() const noexcept { return gap_count_ > 0 && !synced_; }

private:
    std::string           symbol_;
    std::string           norm_;
    bool                  futures_;
    std::optional<int64_t> last_u_;
    bool                  synced_ = false;
    int64_t               gap_count_ = 0;
    std::vector<std::string> pending_diffs_;

    std::vector<L2Update> parse_depth_diff(const std::string& data) {
        if (!synced_) { pending_diffs_.push_back(data); return {}; }

        std::string_view sv(data);
        int64_t U  = detail::json_int64(sv, "U");
        int64_t u  = detail::json_int64(sv, "u");
        int64_t ts = ms_to_ns(detail::json_int64(sv, "T"));

        // Drop stale
        if (last_u_.has_value() && u <= *last_u_) return {};

        // Gap detection
        if (last_u_.has_value() && U > *last_u_ + 1) {
            ++gap_count_;
            synced_ = false;  // need re-snapshot
            return {};
        }

        std::vector<L2Update> out;
        auto bids_arr = detail::extract_named_array(sv, "b");
        auto asks_arr = detail::extract_named_array(sv, "a");

        for (auto& [p, q] : detail::parse_price_qty_array(bids_arr))
            out.push_back({norm_, BookSide::Bid, p, q, ts, u});
        for (auto& [p, q] : detail::parse_price_qty_array(asks_arr))
            out.push_back({norm_, BookSide::Ask, p, q, ts, u});

        last_u_ = u;
        return out;
    }

    Trade parse_agg_trade(std::string_view data) {
        // m=true  → buyer was market-maker → aggressor is SELLER
        bool buyer_mm  = detail::json_bool(data, "m");
        Side aggressor = buyer_mm ? Side::Sell : Side::Buy;
        Trade t;
        t.trade_id  = std::to_string(detail::json_int64(data, "a"));
        t.symbol    = norm_;
        t.side      = aggressor;
        t.price     = detail::json_double(data, "p");
        t.qty       = detail::json_double(data, "q");
        t.timestamp = ms_to_ns(detail::json_int64(data, "T"));
        t.aggressor = aggressor;
        t.buyer_mm  = buyer_mm;
        return t;
    }

    Trade parse_trade(std::string_view data) {
        bool buyer_mm  = detail::json_bool(data, "m");
        Side aggressor = buyer_mm ? Side::Sell : Side::Buy;
        Trade t;
        t.trade_id  = std::to_string(detail::json_int64(data, "t"));
        t.symbol    = norm_;
        t.side      = aggressor;
        t.price     = detail::json_double(data, "p");
        t.qty       = detail::json_double(data, "q");
        t.timestamp = ms_to_ns(detail::json_int64(data, "T"));
        t.aggressor = aggressor;
        t.buyer_mm  = buyer_mm;
        return t;
    }

    std::vector<L2Update> parse_book_ticker(std::string_view data) {
        int64_t ts = current_time_ns();
        return {
            {norm_, BookSide::Bid, detail::json_double(data,"b"), detail::json_double(data,"B"), ts},
            {norm_, BookSide::Ask, detail::json_double(data,"a"), detail::json_double(data,"A"), ts},
        };
    }

    static int64_t current_time_ns() {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(
            system_clock::now().time_since_epoch()).count();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// BybitParser
// ─────────────────────────────────────────────────────────────────────────────
class BybitParser {
public:
    explicit BybitParser(std::string symbol)
        : symbol_(std::move(symbol))
        , norm_(normalise_symbol(symbol_))
    {}

    // ── Parse a Bybit V5 WebSocket message ────────────────────────────────────
    // Pass book pointer to enable CRC32 checksum validation on deltas.
    std::vector<MarketEvent> parse(std::string_view msg_json,
                                   OrderBook* book = nullptr)
    {
        auto topic = detail::json_get(msg_json, "topic");
        std::vector<MarketEvent> out;

        if (topic.find("orderbook") != std::string_view::npos) {
            auto lv = parse_orderbook(msg_json, book);
            for (auto& e : lv) out.push_back(e);
        } else if (topic.find("publicTrade") != std::string_view::npos) {
            auto trades = parse_trades(msg_json);
            for (auto& t : trades) out.push_back(t);
        }
        return out;
    }

    int64_t gap_count() const noexcept { return gap_count_; }

private:
    std::string           symbol_;
    std::string           norm_;
    std::optional<int64_t> prev_seq_;
    int64_t               gap_count_ = 0;

    std::vector<L2Update> parse_orderbook(std::string_view msg,
                                           OrderBook* book)
    {
        auto  mtype  = detail::json_get(msg, "type");
        int64_t ts   = ms_to_ns(detail::json_int64(msg, "ts"));
        int64_t seq  = detail::json_int64(msg, "seq");

        // Sequence validation for deltas
        if (mtype == "delta" && prev_seq_.has_value() && seq != 0) {
            if (seq != *prev_seq_ + 1) {
                ++gap_count_;
            }
        }
        if (seq) prev_seq_ = seq;

        // Extract "data" object
        auto data_pos = msg.find("\"data\"");
        std::string_view data = (data_pos != std::string_view::npos)
                                ? msg.substr(data_pos) : msg;

        auto bids_arr = detail::extract_named_array(data, "b");
        auto asks_arr = detail::extract_named_array(data, "a");

        std::vector<L2Update> out;
        for (auto& [p, q] : detail::parse_price_qty_array(bids_arr))
            out.push_back({norm_, BookSide::Bid, p, q, ts, seq});
        for (auto& [p, q] : detail::parse_price_qty_array(asks_arr))
            out.push_back({norm_, BookSide::Ask, p, q, ts, seq});

        // CRC32 validation
        if (book && mtype == "delta") {
            int64_t cts = detail::json_int64(data, "cts");
            if (cts != 0) {
                book->validate_bybit_checksum(static_cast<uint32_t>(cts));
            }
        }
        return out;
    }

    std::vector<Trade> parse_trades(std::string_view msg) {
        // Bybit trade array: "data":[{"S":"Buy","p":"43500","v":"0.1","T":"...","i":"..."}]
        std::vector<Trade> out;
        auto data_arr = detail::extract_named_array(msg, "data");
        int64_t msg_ts = ms_to_ns(detail::json_int64(msg, "ts"));

        // Walk each object in the array
        size_t pos = 0;
        while (pos < data_arr.size()) {
            pos = data_arr.find('{', pos);
            if (pos == std::string_view::npos) break;
            size_t end = data_arr.find('}', pos);
            if (end == std::string_view::npos) break;
            std::string_view obj = data_arr.substr(pos, end - pos + 1);

            auto side_str = detail::json_get(obj, "S");
            Side side = (side_str == "Buy") ? Side::Buy : Side::Sell;
            int64_t ts_raw = detail::json_int64(obj, "T");
            int64_t ts = ts_raw ? ms_to_ns(ts_raw) : msg_ts;

            Trade t;
            t.trade_id  = std::string(detail::json_get(obj, "i"));
            t.symbol    = norm_;
            t.side      = side;
            t.price     = detail::json_double(obj, "p");
            t.qty       = detail::json_double(obj, "v");
            t.timestamp = ts;
            t.aggressor = side;
            out.push_back(std::move(t));

            pos = end + 1;
        }
        return out;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// CsvL2Reader  —  streams L2Updates from a CSV file (Tardis.dev format)
// Columns: timestamp, side (bid/ask or is_bid bool), price, qty/amount
// ─────────────────────────────────────────────────────────────────────────────
class CsvL2Reader {
public:
    CsvL2Reader(const std::string& filepath,
                std::string symbol,
                std::string ts_col    = "exchange_timestamp",
                std::string side_col  = "side",     // "bid"/"ask" or "true"/"false"
                std::string price_col = "price",
                std::string qty_col   = "amount")
        : symbol_(normalise_symbol(std::move(symbol)))
        , ts_col_(std::move(ts_col))
        , side_col_(std::move(side_col))
        , price_col_(std::move(price_col))
        , qty_col_(std::move(qty_col))
        , file_(filepath)
    {
        if (!file_.is_open())
            throw std::runtime_error("CsvL2Reader: cannot open " + filepath);
        // Read header
        std::string header;
        if (!std::getline(file_, header)) return;
        // Build column index map
        std::istringstream hss(header);
        std::string col; int idx = 0;
        while (std::getline(hss, col, ',')) {
            col.erase(col.find_last_not_of("\r\n ") + 1);
            col_idx_[col] = idx++;
        }
        n_cols_ = idx;
    }

    // Read next event; returns false when stream exhausted
    bool next(L2Update& out) {
        std::string line;
        while (std::getline(file_, line)) {
            if (line.empty()) continue;
            auto cols = split_csv(line, n_cols_);
            if (cols.size() < static_cast<size_t>(n_cols_)) continue;

            auto get = [&](const std::string& name) -> std::string_view {
                auto it = col_idx_.find(name);
                return it != col_idx_.end() ? std::string_view(cols[it->second]) : std::string_view{};
            };

            int64_t ts = 0;
            auto ts_sv = get(ts_col_);
            std::from_chars(ts_sv.data(), ts_sv.data()+ts_sv.size(), ts);

            double price = 0.0, qty = 0.0;
            auto p_sv = get(price_col_), q_sv = get(qty_col_);
            std::from_chars(p_sv.data(), p_sv.data()+p_sv.size(), price);
            std::from_chars(q_sv.data(), q_sv.data()+q_sv.size(), qty);

            auto side_sv = get(side_col_);
            BookSide side = BookSide::Bid;
            if (side_sv == "ask" || side_sv == "false" || side_sv == "0")
                side = BookSide::Ask;

            out = L2Update{symbol_, side, price, qty, ts, 0};
            return true;
        }
        return false;
    }

    // Collect all events
    std::vector<L2Update> read_all() {
        std::vector<L2Update> out;
        L2Update e;
        while (next(e)) out.push_back(e);
        return out;
    }

private:
    std::string  symbol_;
    std::string  ts_col_, side_col_, price_col_, qty_col_;
    std::ifstream file_;
    std::unordered_map<std::string, int> col_idx_;
    int n_cols_ = 0;

    static std::vector<std::string> split_csv(const std::string& line, int expected) {
        std::vector<std::string> cols;
        cols.reserve(expected);
        size_t pos = 0, len = line.size();
        while (pos <= len) {
            size_t next = line.find(',', pos);
            if (next == std::string::npos) next = len;
            cols.push_back(line.substr(pos, next - pos));
            pos = next + 1;
        }
        return cols;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// CsvTradeReader  —  streams Trades from CSV (Tardis.dev format)
// Columns: timestamp, price, amount/qty, side (buy/sell or aggressor)
// ─────────────────────────────────────────────────────────────────────────────
class CsvTradeReader {
public:
    CsvTradeReader(const std::string& filepath,
                   std::string symbol,
                   std::string ts_col    = "exchange_timestamp",
                   std::string price_col = "price",
                   std::string qty_col   = "amount",
                   std::string side_col  = "side",
                   std::string id_col    = "id")
        : symbol_(normalise_symbol(std::move(symbol)))
        , ts_col_(std::move(ts_col))
        , price_col_(std::move(price_col))
        , qty_col_(std::move(qty_col))
        , side_col_(std::move(side_col))
        , id_col_(std::move(id_col))
        , file_(filepath)
    {
        if (!file_.is_open())
            throw std::runtime_error("CsvTradeReader: cannot open " + filepath);
        std::string header;
        if (!std::getline(file_, header)) return;
        std::istringstream hss(header);
        std::string col; int idx = 0;
        while (std::getline(hss, col, ',')) {
            col.erase(col.find_last_not_of("\r\n ") + 1);
            col_idx_[col] = idx++;
        }
        n_cols_ = idx;
    }

    bool next(Trade& out) {
        std::string line;
        while (std::getline(file_, line)) {
            if (line.empty()) continue;
            auto cols = split_csv(line, n_cols_);
            if (cols.size() < static_cast<size_t>(n_cols_)) continue;

            auto get = [&](const std::string& name) -> std::string_view {
                auto it = col_idx_.find(name);
                return it != col_idx_.end() ? std::string_view(cols[it->second]) : std::string_view{};
            };

            int64_t ts = 0;
            auto ts_sv = get(ts_col_);
            std::from_chars(ts_sv.data(), ts_sv.data()+ts_sv.size(), ts);

            double price = 0.0, qty = 0.0;
            auto p_sv = get(price_col_), q_sv = get(qty_col_);
            std::from_chars(p_sv.data(), p_sv.data()+p_sv.size(), price);
            std::from_chars(q_sv.data(), q_sv.data()+q_sv.size(), qty);

            auto side_sv  = get(side_col_);
            auto id_sv    = get(id_col_);

            Side side = Side::Buy;
            if (side_sv == "sell" || side_sv == "Sell" || side_sv == "SELL")
                side = Side::Sell;

            out.trade_id  = id_sv.empty() ? std::to_string(++row_) : std::string(id_sv);
            out.symbol    = symbol_;
            out.side      = side;
            out.price     = price;
            out.qty       = qty;
            out.timestamp = ts;
            out.aggressor = side;
            return true;
        }
        return false;
    }

    std::vector<Trade> read_all() {
        std::vector<Trade> out;
        Trade t;
        while (next(t)) out.push_back(t);
        return out;
    }

private:
    std::string  symbol_;
    std::string  ts_col_, price_col_, qty_col_, side_col_, id_col_;
    std::ifstream file_;
    std::unordered_map<std::string, int> col_idx_;
    int    n_cols_ = 0;
    int64_t row_  = 0;

    static std::vector<std::string> split_csv(const std::string& line, int expected) {
        std::vector<std::string> cols;
        cols.reserve(expected);
        size_t pos = 0, len = line.size();
        while (pos <= len) {
            size_t next = line.find(',', pos);
            if (next == std::string::npos) next = len;
            cols.push_back(line.substr(pos, next - pos));
            pos = next + 1;
        }
        return cols;
    }
};

} // namespace hft