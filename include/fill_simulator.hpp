#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// fill_simulator.hpp  —  Production fill simulation
//
//  • FIFO / pro-rata / hybrid fill modes
//  • Almgren-Chriss market impact on aggressive orders
//  • Adverse selection penalty when toxicity > threshold
//  • Iceberg hidden volume
//  • Self-trade prevention (STP)
//  • IOC / FOK semantics
// ─────────────────────────────────────────────────────────────────────────────

#include "types.hpp"
#include "orderbook.hpp"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <random>
#include <string>
#include <algorithm>
#include <cmath>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// FillModelConfig
// ─────────────────────────────────────────────────────────────────────────────
enum class FillMode : uint8_t { FIFO, ProRata, Hybrid };
enum class STPMode  : uint8_t { CancelMaker, CancelTaker, None };

struct FillModelConfig {
    double   maker_fee       = -0.0002;
    double   taker_fee       =  0.0004;
    FillMode fill_mode       = FillMode::FIFO;
    double   min_qty_fifo    = 0.0;        // hybrid: FIFO below this, pro-rata above
    STPMode  stp_mode        = STPMode::CancelMaker;
    double   iceberg_prob    = 0.0;        // 0 = no icebergs
    double   iceberg_hidden  = 3.0;        // hidden:visible ratio
    double   adverse_penalty = 0.5;        // bps added to taker price when toxic
    double   adverse_thresh  = 0.70;       // PIN > this triggers penalty
    double   ac_eta          = 1e-7;       // Almgren-Chriss permanent impact
    double   ac_gamma        = 1e-6;       // Almgren-Chriss temporary impact
};

// ─────────────────────────────────────────────────────────────────────────────
// FillSimulator
// ─────────────────────────────────────────────────────────────────────────────
class FillSimulator {
public:
    explicit FillSimulator(FillModelConfig cfg = {}, uint64_t seed = 42)
        : cfg_(std::move(cfg)), rng_(seed), uniform_(0.0, 1.0) {}

    // ── Market order fill ────────────────────────────────────────────────────

    std::vector<FillEvent> fill_market_order(
        Order&      order,
        OrderBook&  book,
        int64_t     timestamp,
        double      toxicity = 0.0)
    {
        std::vector<FillEvent> fills;
        double remaining = order.qty - order.filled_qty;
        bool is_buy      = (order.side == Side::Buy);

        auto levels = is_buy ? book.ask_depth(200) : book.bid_depth(200);
        bool fok_fail = false;

        if (order.order_type == OrderType::FOK) {
            // Check total available before filling
            double avail = 0.0;
            for (auto& lv : levels) avail += lv.qty;
            if (avail < remaining - 1e-12) fok_fail = true;
        }

        if (!fok_fail) {
            for (auto& lv : levels) {
                if (remaining <= 1e-12) break;
                double fill_qty = std::min(lv.qty, remaining);

                // Almgren-Chriss impact
                double notional    = fill_qty * lv.price;
                double temp_impact = cfg_.ac_gamma * notional;
                double perm_impact = cfg_.ac_eta   * notional;
                double sign        = is_buy ? 1.0 : -1.0;
                double fill_price  = lv.price + sign * (temp_impact + perm_impact * 0.5);

                // Adverse selection penalty
                if (toxicity > cfg_.adverse_thresh) {
                    double penalty = fill_price * (cfg_.adverse_penalty / 10000.0);
                    fill_price += sign * penalty;
                }

                double fee = fill_qty * fill_price * cfg_.taker_fee;

                FillEvent fe;
                fe.order_id      = order.order_id;
                fe.symbol        = order.symbol;
                fe.side          = order.side;
                fe.price         = fill_price;
                fe.qty           = fill_qty;
                fe.timestamp     = timestamp;
                fe.is_maker      = false;
                fe.fee_rate      = cfg_.taker_fee;
                fe.fee           = fee;
                fe.adverse_score = toxicity;
                fills.push_back(std::move(fe));

                remaining       -= fill_qty;
                
            }
        }

        // Update order state
        for (auto& f : fills) order.filled_qty += f.qty;

        if (order.filled_qty >= order.qty - 1e-12) {
            order.status = OrderStatus::Filled;
        } else if (order.filled_qty > 0.0) {
            if (order.order_type == OrderType::IOC) {
                order.status = OrderStatus::Cancelled;
            } else if (order.order_type == OrderType::FOK || fok_fail) {
                fills.clear();
                order.filled_qty = 0.0;
                order.status     = OrderStatus::Cancelled;
            } else {
                order.status = OrderStatus::Partial;
            }
        }
        return fills;
    }

    // ── Limit / post-only fill (queue-aware) ─────────────────────────────────

    std::vector<FillEvent> on_public_trade(
        const Trade&                              trade,
        std::unordered_map<std::string, Order*>&  resting_orders,
        OrderBook&                                book,
        double                                    toxicity = 0.0)
    {
        std::vector<FillEvent> fills;
        if (resting_orders.empty()) return fills;

        for (auto& [oid, order_ptr] : resting_orders) {
            Order& order = *order_ptr;
            if (order.status != OrderStatus::Open && order.status != OrderStatus::Partial)
                continue;
            if (order.symbol != trade.symbol) continue;
            if (order.order_type == OrderType::Market) continue;

            // STP check
            if (cfg_.stp_mode != STPMode::None &&
                our_trade_ids_.count(trade.trade_id)) {
                if (cfg_.stp_mode == STPMode::CancelMaker) {
                    order.status = OrderStatus::Cancelled;
                    book.cancel_our_order(order);
                }
                continue;
            }

            // Did this trade sweep our level?
            bool swept =
                (order.side == Side::Buy  && trade.aggressor == Side::Sell && trade.price <= order.price) ||
                (order.side == Side::Sell && trade.aggressor == Side::Buy  && trade.price >= order.price);
            if (!swept) continue;

            bool     is_bid = (order.side == Side::Buy);
            PriceLevel* lvl = book.get_level(order.price, is_bid);
            if (!lvl) continue;

            double ahead     = book.qty_ahead_of_order(order);
            double trade_vol = trade.qty;
            double fill_qty  = 0.0;

            if (cfg_.fill_mode == FillMode::FIFO ||
                lvl->total_qty <= cfg_.min_qty_fifo) {
                fill_qty = fifo_fill(*lvl, ahead, trade_vol,
                                     order, book, is_bid);
            } else if (cfg_.fill_mode == FillMode::ProRata) {
                fill_qty = prorata_fill(*lvl, trade_vol, order);
            } else {
                // Hybrid: take the better of FIFO and pro-rata
                double f1 = fifo_fill(*lvl, ahead, trade_vol, order, book, is_bid);
                double f2 = prorata_fill(*lvl, trade_vol, order);
                fill_qty  = std::max(f1, f2);
            }

            if (fill_qty <= 1e-12) continue;

            // Iceberg absorption
            if (cfg_.iceberg_prob > 0.0 && uniform_(rng_) < cfg_.iceberg_prob) {
                fill_qty /= (1.0 + cfg_.iceberg_hidden * uniform_(rng_));
            }

            fill_qty = std::min(fill_qty, order.qty - order.filled_qty);
            if (fill_qty <= 1e-12) continue;

            double fee = fill_qty * order.price * cfg_.maker_fee;

            FillEvent fe;
            fe.order_id      = order.order_id;
            fe.symbol        = order.symbol;
            fe.side          = order.side;
            fe.price         = order.price;
            fe.qty           = fill_qty;
            fe.timestamp     = trade.timestamp;
            fe.is_maker      = true;
            fe.fee_rate      = cfg_.maker_fee;
            fe.fee           = fee;
            fe.adverse_score = toxicity;
            fills.push_back(fe);

            order.filled_qty += fill_qty;
            if (order.filled_qty >= order.qty - 1e-12) {
                order.status = OrderStatus::Filled;
                book.cancel_our_order(order);
            } else {
                order.status = OrderStatus::Partial;
            }
        }
        return fills;
    }

    // Post-only validation: returns true if order will rest (not cross)
    bool check_post_only(const Order& order, const OrderBook& book) const noexcept {
        if (order.side == Side::Buy) {
            auto ba = book.best_ask();
            return !ba || order.price < *ba;
        } else {
            auto bb = book.best_bid();
            return !bb || order.price > *bb;
        }
    }

    void register_our_trade(const std::string& trade_id) {
        our_trade_ids_.insert(trade_id);
    }

    const FillModelConfig& config() const noexcept { return cfg_; }

private:
    FillModelConfig                  cfg_;
    std::mt19937_64                  rng_;
    std::uniform_real_distribution<> uniform_;
    std::unordered_set<std::string>  our_trade_ids_;

    double fifo_fill(PriceLevel& lvl, double ahead, double trade_vol,
                     Order& order, OrderBook& book, bool is_bid) {
        if (ahead >= trade_vol) {
            lvl.consume_qty(trade_vol);
            return 0.0;
        }
        lvl.consume_qty(ahead);
        return std::min(trade_vol - ahead, order.qty - order.filled_qty);
    }

    double prorata_fill(PriceLevel& lvl, double trade_vol, const Order& order) const noexcept {
        if (lvl.total_qty <= 1e-12) return 0.0;
        double rem   = order.qty - order.filled_qty;
        double share = rem / lvl.total_qty;
        return std::min(trade_vol * share, rem);
    }
};

} // namespace hft