#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// types.hpp  —  Core domain types for the HFT backtesting engine
//
// All timestamps are int64_t nanoseconds (Unix epoch).
// All quantities and prices are double-precision IEEE 754.
// Strings are std::string; hot-path comparisons use enums where possible.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string>
#include <optional>

namespace hft {

// ── Side ─────────────────────────────────────────────────────────────────────
enum class Side : uint8_t { Buy, Sell, Unknown };

inline Side side_from_str(const std::string& s) noexcept {
    if (s == "buy"  || s == "Buy"  || s == "BUY")  return Side::Buy;
    if (s == "sell" || s == "Sell" || s == "SELL") return Side::Sell;
    return Side::Unknown;
}
inline const char* side_to_str(Side s) noexcept {
    switch (s) {
        case Side::Buy:  return "buy";
        case Side::Sell: return "sell";
        default:         return "unknown";
    }
}

// ── Order type ────────────────────────────────────────────────────────────────
enum class OrderType : uint8_t { Limit, Market, PostOnly, IOC, FOK };

// ── Order status ──────────────────────────────────────────────────────────────
enum class OrderStatus : uint8_t { Open, Partial, Filled, Cancelled, Rejected };

inline const char* status_to_str(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::Open:      return "open";
        case OrderStatus::Partial:   return "partial";
        case OrderStatus::Filled:    return "filled";
        case OrderStatus::Cancelled: return "cancelled";
        case OrderStatus::Rejected:  return "rejected";
        default:                     return "unknown";
    }
}

// ── BookSide ──────────────────────────────────────────────────────────────────
enum class BookSide : uint8_t { Bid, Ask };

// ── L3 event type ─────────────────────────────────────────────────────────────
enum class L3Event : uint8_t { Add, Modify, Delete, Trade };

// ─────────────────────────────────────────────────────────────────────────────
// Order
// ─────────────────────────────────────────────────────────────────────────────
struct Order {
    std::string  order_id;
    std::string  symbol;
    Side         side        = Side::Buy;
    double       price       = 0.0;
    double       qty         = 0.0;
    int64_t      timestamp   = 0;      // ns
    OrderType    order_type  = OrderType::Limit;
    double       filled_qty  = 0.0;
    OrderStatus  status      = OrderStatus::Open;
    int32_t      queue_pos   = 0;
    double       queue_ahead = 0.0;
    std::string  client_id;
};

// ─────────────────────────────────────────────────────────────────────────────
// Trade  (public market trade)
// ─────────────────────────────────────────────────────────────────────────────
struct Trade {
    std::string  trade_id;
    std::string  symbol;
    Side         side;
    double       price       = 0.0;
    double       qty         = 0.0;
    int64_t      timestamp   = 0;
    Side         aggressor   = Side::Buy;
    bool         buyer_mm    = false;   // Binance: buyer was market-maker
};

// ─────────────────────────────────────────────────────────────────────────────
// L2Update  (price-level diff)
// ─────────────────────────────────────────────────────────────────────────────
struct L2Update {
    std::string  symbol;
    BookSide     side;
    double       price     = 0.0;
    double       qty       = 0.0;   // 0 → remove level
    int64_t      timestamp = 0;
    int64_t      seq       = 0;     // exchange sequence number
};

// ─────────────────────────────────────────────────────────────────────────────
// L3Update  (individual order event)
// ─────────────────────────────────────────────────────────────────────────────
struct L3Update {
    std::string  symbol;
    L3Event      event;
    std::string  order_id;
    Side         side;
    double       price     = 0.0;
    double       qty       = 0.0;
    int64_t      timestamp = 0;
    int64_t      seq       = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// FillEvent
// ─────────────────────────────────────────────────────────────────────────────
struct FillEvent {
    std::string  order_id;
    std::string  symbol;
    Side         side;
    double       price         = 0.0;
    double       qty           = 0.0;
    int64_t      timestamp     = 0;
    bool         is_maker      = false;
    double       fee_rate      = 0.0;
    double       fee           = 0.0;
    double       realized_pnl  = 0.0;
    double       adverse_score = 0.0;
};

// ─────────────────────────────────────────────────────────────────────────────
// DepthLevel  (price + qty snapshot for N-level depth snapshots)
// ─────────────────────────────────────────────────────────────────────────────
struct DepthLevel {
    double price = 0.0;
    double qty   = 0.0;
};

// ─────────────────────────────────────────────────────────────────────────────
// MarketEvent  —  type-erased event wrapper for the event queue
// ─────────────────────────────────────────────────────────────────────────────
enum class EventKind : uint8_t {
    L2,             // market data: L2Update
    L3,             // market data: L3Update
    PublicTrade,    // market data: Trade
    OrderAck,       // order lifecycle: ack / reject
    FillDelivery,   // order lifecycle: fill notification to strategy
};

} // namespace hft