#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// execution.hpp  —  Multi-leg execution algorithms
// TWAP, Iceberg, POV (Percentage of Volume)
// ─────────────────────────────────────────────────────────────────────────────

#include "simulator.hpp"
#include "signals.hpp"
#include <string>
#include <deque>
#include <cmath>
#include <optional>
#include <algorithm>
#include <random>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// ParentOrder  —  the large order we want to execute
// ─────────────────────────────────────────────────────────────────────────────
struct ParentOrder {
    std::string  order_id;
    std::string  symbol;
    Side         side;
    double       total_qty;
    double       limit_price   = 0.0;
    int64_t      start_ns      = 0;
    int64_t      end_ns        = 0;
    double       filled_qty    = 0.0;
    bool         complete      = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Configs — defined outside executor classes to avoid GCC nested-struct issue
// ─────────────────────────────────────────────────────────────────────────────
struct TwapConfig {
    int    n_slices          = 20;
    double max_participation = 0.30;
    double price_offset_bps  = 2.0;
    bool   use_limit_orders  = true;
    double slice_randomize   = 0.20;
};

struct IcebergConfig {
    double visible_qty       = 0.01;
    double price_offset_bps  = 1.0;
    bool   randomize_qty     = true;
    bool   randomize_price   = true;
};

struct POVConfig {
    double pov_rate          = 0.10;
    double price_offset_bps  = 0.5;
    double min_child_qty     = 0.001;
    double max_child_qty     = 0.10;
};

// ─────────────────────────────────────────────────────────────────────────────
// TwapExecutor
// ─────────────────────────────────────────────────────────────────────────────
class TwapExecutor : public Strategy {
public:
    TwapExecutor(ParentOrder parent, TwapConfig cfg = TwapConfig{})
        : parent_(std::move(parent)), cfg_(cfg)
    {
        slice_qty_base_ = parent_.total_qty / std::max(cfg_.n_slices, 1);
        int64_t dur     = parent_.end_ns - parent_.start_ns;
        slice_duration_ = dur / std::max(cfg_.n_slices, 1);
        next_slice_at_  = parent_.start_ns;
    }

    void on_book_update(const std::string& symbol,
                        OrderBook& book,
                        int64_t ts_ns,
                        SimEngine& engine) override
    {
        if (parent_.complete || symbol != parent_.symbol) return;
        if (ts_ns < next_slice_at_ || ts_ns > parent_.end_ns) return;

        next_slice_at_ += slice_duration_;

        double remaining = parent_.total_qty - parent_.filled_qty;
        if (remaining <= 1e-9) { parent_.complete = true; return; }

        double rand_mult = 1.0 + cfg_.slice_randomize * ((rng_() % 100) / 50.0 - 1.0);
        double slice_qty = std::min(remaining, slice_qty_base_ * rand_mult);

        auto depth = (parent_.side == Side::Buy) ? book.ask_depth(3) : book.bid_depth(3);
        double avail = 0.0;
        for (auto& lv : depth) avail += lv.qty;
        if (avail > 1e-9)
            slice_qty = std::min(slice_qty, avail * cfg_.max_participation);
        if (slice_qty < 1e-9) return;

        if (cfg_.use_limit_orders) {
            auto mp = book.mid_price();
            if (!mp) return;
            double bps   = cfg_.price_offset_bps / 10000.0;
            double price = (parent_.side == Side::Buy)
                ? signals::round_to_tick(*mp * (1.0 + bps), tick_size_)
                : signals::round_to_tick(*mp * (1.0 - bps), tick_size_);
            if (parent_.limit_price > 1e-9) {
                if (parent_.side == Side::Buy  && price > parent_.limit_price) return;
                if (parent_.side == Side::Sell && price < parent_.limit_price) return;
            }
            engine.submit_limit(parent_.symbol, parent_.side, price, slice_qty, false);
        } else {
            engine.submit_market(parent_.symbol, parent_.side, slice_qty);
        }
        ++slices_sent_;
    }

    void on_fill(const FillEvent& fill, PortfolioState&, int64_t, SimEngine&) override {
        if (fill.symbol == parent_.symbol) {
            parent_.filled_qty += fill.qty;
            if (parent_.filled_qty >= parent_.total_qty - 1e-9) parent_.complete = true;
        }
    }

    void set_tick_size(double ts) { tick_size_ = ts; }
    const ParentOrder& parent()   const { return parent_; }
    int    slices_sent()          const { return slices_sent_; }
    double fill_pct()             const {
        return parent_.total_qty > 0
            ? parent_.filled_qty / parent_.total_qty * 100.0 : 0.0;
    }

private:
    ParentOrder  parent_;
    TwapConfig   cfg_;
    double       slice_qty_base_ = 0.0;
    int64_t      slice_duration_ = 0;
    int64_t      next_slice_at_  = 0;
    int          slices_sent_    = 0;
    double       tick_size_      = 0.01;
    std::mt19937_64 rng_{42};
};

// ─────────────────────────────────────────────────────────────────────────────
// IcebergExecutor
// ─────────────────────────────────────────────────────────────────────────────
class IcebergExecutor : public Strategy {
public:
    IcebergExecutor(ParentOrder parent, IcebergConfig cfg = IcebergConfig{})
        : parent_(std::move(parent)), cfg_(cfg) {}

    void on_book_update(const std::string& symbol,
                        OrderBook& book,
                        int64_t, SimEngine& engine) override
    {
        if (parent_.complete || symbol != parent_.symbol) return;
        if (live_order_id_.has_value()) return;

        double remaining = parent_.total_qty - parent_.filled_qty;
        if (remaining <= 1e-9) { parent_.complete = true; return; }

        auto mp = book.mid_price();
        if (!mp) return;

        double qty = std::min(remaining, cfg_.visible_qty);
        if (cfg_.randomize_qty)
            qty *= (0.8 + 0.4 * ((rng_() % 100) / 100.0));
        qty = std::min(qty, remaining);
        if (qty < 1e-9) return;

        double bps   = cfg_.price_offset_bps / 10000.0;
        double price = (parent_.side == Side::Buy)
            ? signals::round_to_tick(*mp * (1.0 + bps), tick_size_)
            : signals::round_to_tick(*mp * (1.0 - bps), tick_size_);
        if (cfg_.randomize_price) {
            int ticks = static_cast<int>(rng_() % 3) - 1;
            price += ticks * tick_size_ * (parent_.side == Side::Buy ? 1.0 : -1.0);
        }
        if (parent_.limit_price > 1e-9) {
            if (parent_.side == Side::Buy  && price > parent_.limit_price) return;
            if (parent_.side == Side::Sell && price < parent_.limit_price) return;
        }

        live_order_id_ = engine.submit_limit(
            parent_.symbol, parent_.side, price, qty, true);
        ++slices_sent_;
    }

    void on_fill(const FillEvent& fill, PortfolioState&, int64_t, SimEngine&) override {
        if (fill.symbol != parent_.symbol) return;
        parent_.filled_qty += fill.qty;
        if (fill.order_id == live_order_id_.value_or("")) live_order_id_.reset();
        if (parent_.filled_qty >= parent_.total_qty - 1e-9) parent_.complete = true;
    }

    void on_order_ack(const Order& order, bool accepted, int64_t, SimEngine&) override {
        if (!accepted && live_order_id_ == order.order_id) live_order_id_.reset();
    }

    void set_tick_size(double ts) { tick_size_ = ts; }
    const ParentOrder& parent() const { return parent_; }
    int    slices_sent()        const { return slices_sent_; }
    double fill_pct()           const {
        return parent_.total_qty > 0
            ? parent_.filled_qty / parent_.total_qty * 100.0 : 0.0;
    }

private:
    ParentOrder  parent_;
    IcebergConfig cfg_;
    std::optional<std::string> live_order_id_;
    int    slices_sent_ = 0;
    double tick_size_   = 0.01;
    std::mt19937_64 rng_{99};
};

// ─────────────────────────────────────────────────────────────────────────────
// POVExecutor  —  Percentage of Volume
// ─────────────────────────────────────────────────────────────────────────────
class POVExecutor : public Strategy {
public:
    POVExecutor(ParentOrder parent, POVConfig cfg = POVConfig{})
        : parent_(std::move(parent)), cfg_(cfg) {}

    void on_trade(const Trade& trade, OrderBook& book,
                  int64_t, SimEngine& engine) override
    {
        if (parent_.complete || trade.symbol != parent_.symbol) return;
        if (live_order_id_.has_value()) return;

        double remaining = parent_.total_qty - parent_.filled_qty;
        if (remaining <= 1e-9) { parent_.complete = true; return; }

        double child_qty = std::clamp(
            trade.qty * cfg_.pov_rate,
            cfg_.min_child_qty,
            cfg_.max_child_qty);
        child_qty = std::min(child_qty, remaining);
        if (child_qty < 1e-9) return;

        auto mp = book.mid_price();
        if (!mp) return;

        double bps   = cfg_.price_offset_bps / 10000.0;
        double price = (parent_.side == Side::Buy)
            ? signals::round_to_tick(*mp * (1.0 + bps), tick_size_)
            : signals::round_to_tick(*mp * (1.0 - bps), tick_size_);

        if (parent_.limit_price > 1e-9) {
            if (parent_.side == Side::Buy  && price > parent_.limit_price) return;
            if (parent_.side == Side::Sell && price < parent_.limit_price) return;
        }

        live_order_id_ = engine.submit_limit(
            parent_.symbol, parent_.side, price, child_qty, true);
    }

    void on_fill(const FillEvent& fill, PortfolioState&, int64_t, SimEngine&) override {
        if (fill.symbol != parent_.symbol) return;
        parent_.filled_qty += fill.qty;
        if (fill.order_id == live_order_id_.value_or("")) live_order_id_.reset();
        if (parent_.filled_qty >= parent_.total_qty - 1e-9) parent_.complete = true;
    }

    void on_order_ack(const Order& order, bool accepted, int64_t, SimEngine&) override {
        if (!accepted && live_order_id_ == order.order_id) live_order_id_.reset();
    }

    void set_tick_size(double ts) { tick_size_ = ts; }
    const ParentOrder& parent() const { return parent_; }
    double fill_pct()           const {
        return parent_.total_qty > 0
            ? parent_.filled_qty / parent_.total_qty * 100.0 : 0.0;
    }

private:
    ParentOrder parent_;
    POVConfig   cfg_;
    std::optional<std::string> live_order_id_;
    double tick_size_ = 0.01;
};

} // namespace hft