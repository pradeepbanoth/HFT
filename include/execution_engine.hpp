#pragma once
// execution_engine.hpp — advanced parent/child execution algorithms

#include "types.hpp"
#include "orderbook.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

enum class ExecAlgo : uint8_t { TWAP, VWAP, POV, Iceberg, Adaptive, Sniper };
enum class ExecState : uint8_t { Created, Running, Paused, Completed, Cancelled, Rejected };
enum class ExecUrgency : uint8_t { Passive, Normal, Aggressive };

struct ExecConfig {
    ExecAlgo algo = ExecAlgo::TWAP;
    ExecUrgency urgency = ExecUrgency::Normal;

    std::string parent_id;
    std::string symbol;
    Side side = Side::Buy;

    double total_qty = 0.0;
    double min_child_qty = 0.001;
    double max_child_qty = 0.05;
    double visible_qty = 0.01;

    int64_t start_ts = 0;
    int64_t end_ts = 0;
    int64_t slice_interval_ns = 1'000'000'000LL;

    double limit_price = 0.0;
    bool post_only = true;

    double pov_rate = 0.05;
    double max_spread_bps = 20.0;
    double min_liquidity_qty = 0.0;
    double aggression = 1.0;

    double max_participation_rate = 0.15;
    double catchup_factor = 1.25;
};

struct MarketContext {
    int64_t ts = 0;
    double recent_market_volume = 0.0;
    double toxicity = 0.0;
    double volatility = 0.0;
};

struct ChildOrderIntent {
    std::string parent_id;
    std::string client_id;
    std::string symbol;
    Side side = Side::Buy;
    OrderType order_type = OrderType::Limit;
    double price = 0.0;
    double qty = 0.0;
    bool post_only = true;
};

struct ExecReport {
    std::string parent_id;
    ExecState state = ExecState::Created;
    double target_qty = 0.0;
    double sent_qty = 0.0;
    double acknowledged_qty = 0.0;
    double filled_qty = 0.0;
    double cancelled_qty = 0.0;
    double live_qty = 0.0;
    double remaining_qty = 0.0;
    double avg_fill_price = 0.0;
    double notional = 0.0;
    int64_t child_count = 0;
    int64_t rejects = 0;
};

class ExecutionEngine {
public:
    bool add_parent(const ExecConfig& cfg) {
        if (cfg.parent_id.empty() || cfg.symbol.empty() || cfg.total_qty <= 0.0) return false;
        if (parents_.count(cfg.parent_id)) return false;

        Parent p;
        p.cfg = cfg;
        p.state = ExecState::Created;
        p.remaining_qty = cfg.total_qty;
        parents_[cfg.parent_id] = std::move(p);
        return true;
    }

    bool start(const std::string& id, int64_t ts) {
        auto* p = get(id);
        if (!p) return false;
        if (p->state != ExecState::Created && p->state != ExecState::Paused) return false;

        p->state = ExecState::Running;
        p->last_slice_ts = ts - p->cfg.slice_interval_ns;
        if (p->cfg.start_ts == 0) p->cfg.start_ts = ts;
        return true;
    }

    bool pause(const std::string& id) {
        auto* p = get(id);
        if (!p || p->state != ExecState::Running) return false;
        p->state = ExecState::Paused;
        return true;
    }

    bool resume(const std::string& id, int64_t ts) {
        auto* p = get(id);
        if (!p || p->state != ExecState::Paused) return false;
        p->state = ExecState::Running;
        p->last_slice_ts = ts - p->cfg.slice_interval_ns;
        return true;
    }

    bool cancel_parent(const std::string& id) {
        auto* p = get(id);
        if (!p) return false;
        p->state = ExecState::Cancelled;
        return true;
    }

    std::vector<ChildOrderIntent> on_timer(
        const MarketContext& ctx,
        const std::unordered_map<std::string, OrderBook*>& books
    ) {
        std::vector<ChildOrderIntent> out;

        for (auto& [id, p] : parents_) {
            if (p.state != ExecState::Running) continue;

            refresh_state(p);

            if (p.remaining_qty <= 1e-12) {
                p.state = ExecState::Completed;
                continue;
            }

            if (p.cfg.end_ts > 0 && ctx.ts > p.cfg.end_ts && p.live_qty <= 1e-12) {
                p.state = ExecState::Completed;
                continue;
            }

            if (ctx.ts - p.last_slice_ts < p.cfg.slice_interval_ns) continue;

            auto bit = books.find(p.cfg.symbol);
            if (bit == books.end() || !bit->second) continue;

            auto child = make_child(p, ctx, *bit->second);
            if (!child) continue;

            register_child(p, *child, ctx.ts);
            out.push_back(*child);
        }

        return out;
    }

    void on_child_ack(const std::string& client_id, bool accepted) {
        auto it = child_to_parent_.find(client_id);
        if (it == child_to_parent_.end()) return;

        auto* p = get(it->second);
        if (!p) return;

        auto cit = p->children.find(client_id);
        if (cit == p->children.end()) return;

        ChildState& c = cit->second;
        if (accepted) {
            c.accepted = true;
            p->acknowledged_qty += c.qty;
        } else {
            c.rejected = true;
            p->rejected_children++;
            p->sent_qty = std::max(0.0, p->sent_qty - c.qty);
        }

        refresh_state(*p);
    }

    void on_child_cancelled(const std::string& client_id) {
        auto it = child_to_parent_.find(client_id);
        if (it == child_to_parent_.end()) return;

        auto* p = get(it->second);
        if (!p) return;

        auto cit = p->children.find(client_id);
        if (cit == p->children.end()) return;

        ChildState& c = cit->second;
        if (!c.cancelled) {
            c.cancelled = true;
            p->cancelled_qty += std::max(0.0, c.qty - c.filled_qty);
        }

        refresh_state(*p);
    }

    void on_fill(const FillEvent& fill) {
        auto it = child_to_parent_.find(fill.order_id);
        if (it == child_to_parent_.end()) return;

        auto* p = get(it->second);
        if (!p) return;

        auto cit = p->children.find(fill.order_id);
        if (cit == p->children.end()) return;

        ChildState& c = cit->second;
        double apply = std::min(fill.qty, std::max(0.0, c.qty - c.filled_qty));
        if (apply <= 1e-12) return;

        c.filled_qty += apply;
        p->filled_qty += apply;
        p->notional += apply * fill.price;
        p->avg_fill_price = p->filled_qty > 1e-12 ? p->notional / p->filled_qty : 0.0;

        refresh_state(*p);
        if (p->filled_qty >= p->cfg.total_qty - 1e-12)
            p->state = ExecState::Completed;
    }

    std::optional<ExecReport> report(const std::string& id) const {
        auto it = parents_.find(id);
        if (it == parents_.end()) return std::nullopt;
        const Parent& p = it->second;

        ExecReport r;
        r.parent_id = p.cfg.parent_id;
        r.state = p.state;
        r.target_qty = p.cfg.total_qty;
        r.sent_qty = p.sent_qty;
        r.acknowledged_qty = p.acknowledged_qty;
        r.filled_qty = p.filled_qty;
        r.cancelled_qty = p.cancelled_qty;
        r.live_qty = p.live_qty;
        r.remaining_qty = std::max(0.0, p.cfg.total_qty - p.filled_qty);
        r.avg_fill_price = p.avg_fill_price;
        r.notional = p.notional;
        r.child_count = p.child_count;
        r.rejects = p.rejected_children;
        return r;
    }

private:
    struct ChildState {
        std::string client_id;
        double qty = 0.0;
        double price = 0.0;
        double filled_qty = 0.0;
        bool accepted = false;
        bool rejected = false;
        bool cancelled = false;
        int64_t created_ts = 0;
    };

    struct Parent {
        ExecConfig cfg;
        ExecState state = ExecState::Created;

        double sent_qty = 0.0;
        double acknowledged_qty = 0.0;
        double filled_qty = 0.0;
        double cancelled_qty = 0.0;
        double live_qty = 0.0;
        double remaining_qty = 0.0;
        double notional = 0.0;
        double avg_fill_price = 0.0;

        int64_t last_slice_ts = 0;
        int64_t child_count = 0;
        int64_t rejected_children = 0;

        std::unordered_map<std::string, ChildState> children;
    };

    std::unordered_map<std::string, Parent> parents_;
    std::unordered_map<std::string, std::string> child_to_parent_;

    Parent* get(const std::string& id) {
        auto it = parents_.find(id);
        return it == parents_.end() ? nullptr : &it->second;
    }

    static void refresh_state(Parent& p) {
        p.live_qty = 0.0;

        for (auto& [id, c] : p.children) {
            if (!c.cancelled && !c.rejected && c.filled_qty < c.qty - 1e-12) {
                p.live_qty += c.qty - c.filled_qty;
            }
        }

        p.remaining_qty = std::max(0.0, p.cfg.total_qty - p.filled_qty - p.live_qty);
    }

    void register_child(Parent& p, const ChildOrderIntent& child, int64_t ts) {
        ChildState c;
        c.client_id = child.client_id;
        c.qty = child.qty;
        c.price = child.price;
        c.created_ts = ts;

        p.children[child.client_id] = c;
        child_to_parent_[child.client_id] = p.cfg.parent_id;

        p.sent_qty += child.qty;
        p.live_qty += child.qty;
        p.remaining_qty = std::max(0.0, p.cfg.total_qty - p.filled_qty - p.live_qty);
        p.child_count++;
        p.last_slice_ts = ts;
    }

    std::optional<ChildOrderIntent> make_child(Parent& p, const MarketContext& ctx, OrderBook& book) {
        if (!market_ok(p.cfg, ctx, book)) return std::nullopt;

        double qty = desired_qty(p, ctx, book);
        qty = std::min(qty, p.remaining_qty);

        if (qty < p.cfg.min_child_qty) {
            if (p.remaining_qty >= p.cfg.min_child_qty) {
                qty = p.cfg.min_child_qty;
            } else {
                qty = p.remaining_qty;
            }
        }

        qty = std::clamp(qty, 0.0, p.cfg.max_child_qty);
        if (qty <= 1e-12) return std::nullopt;

        double price = choose_price(p.cfg, book);
        if (price <= 0.0) return std::nullopt;

        ChildOrderIntent child;
        child.parent_id = p.cfg.parent_id;
        child.client_id = p.cfg.parent_id + "_child_" + std::to_string(p.child_count + 1);
        child.symbol = p.cfg.symbol;
        child.side = p.cfg.side;
        child.qty = qty;
        child.price = price;
        child.post_only = p.cfg.post_only;
        child.order_type = p.cfg.post_only ? OrderType::PostOnly : OrderType::Limit;
        return child;
    }

    static bool market_ok(const ExecConfig& cfg, const MarketContext& ctx, const OrderBook& book) {
        if (ctx.toxicity >= 0.85) return false;

        auto sp = book.spread_bps();
        if (sp && *sp > cfg.max_spread_bps && cfg.urgency != ExecUrgency::Aggressive)
            return false;

        double depth = 0.0;
        auto levels = cfg.side == Side::Buy ? book.ask_depth(5) : book.bid_depth(5);
        for (const auto& lv : levels) depth += lv.qty;

        if (cfg.min_liquidity_qty > 0.0 && depth < cfg.min_liquidity_qty)
            return false;

        return true;
    }

    static double desired_qty(const Parent& p, const MarketContext& ctx, const OrderBook& book) {
        double q = 0.0;

        switch (p.cfg.algo) {
            case ExecAlgo::TWAP: q = twap_qty(p, ctx.ts); break;
            case ExecAlgo::VWAP: q = vwap_qty(p, ctx.ts); break;
            case ExecAlgo::POV: q = ctx.recent_market_volume * p.cfg.pov_rate; break;
            case ExecAlgo::Iceberg: q = std::min(p.cfg.visible_qty, p.remaining_qty); break;
            case ExecAlgo::Adaptive: q = adaptive_qty(p, ctx, book); break;
            case ExecAlgo::Sniper: q = sniper_qty(p, book); break;
        }

        double max_by_participation = ctx.recent_market_volume > 1e-12
            ? ctx.recent_market_volume * p.cfg.max_participation_rate
            : p.cfg.max_child_qty;

        if (p.cfg.algo == ExecAlgo::TWAP ||
            p.cfg.algo == ExecAlgo::VWAP ||
            p.cfg.algo == ExecAlgo::Iceberg ||
            p.cfg.algo == ExecAlgo::Sniper) {
            max_by_participation = p.cfg.max_child_qty;
        }

        q = std::min(q, max_by_participation);
        q *= urgency_mult(p.cfg.urgency);
        return q;
    }

    static double twap_qty(const Parent& p, int64_t ts) {
        if (p.cfg.end_ts <= p.cfg.start_ts)
            return p.cfg.max_child_qty;

        int64_t remaining_time = std::max<int64_t>(1, p.cfg.end_ts - ts);
        double slices_left = std::max(1.0, static_cast<double>(remaining_time) / p.cfg.slice_interval_ns);
        double ideal = p.remaining_qty / slices_left;

        double elapsed = static_cast<double>(ts - p.cfg.start_ts);
        double duration = static_cast<double>(p.cfg.end_ts - p.cfg.start_ts);
        double target_progress = std::clamp(elapsed / duration, 0.0, 1.0);
        double target_filled = p.cfg.total_qty * target_progress;
        double behind = std::max(0.0, target_filled - p.filled_qty);

        return std::max(ideal, behind * p.cfg.catchup_factor);
    }

    static double vwap_qty(const Parent& p, int64_t ts) {
        if (p.cfg.end_ts <= p.cfg.start_ts)
            return p.cfg.max_child_qty;

        double progress = static_cast<double>(ts - p.cfg.start_ts) /
                          static_cast<double>(p.cfg.end_ts - p.cfg.start_ts);
        progress = std::clamp(progress, 0.0, 1.0);

        double curve = 0.5 - 0.5 * std::cos(progress * 3.14159265358979323846);
        double target = p.cfg.total_qty * curve;
        return std::max(0.0, target - p.filled_qty - p.live_qty);
    }

    static double adaptive_qty(const Parent& p, const MarketContext& ctx, const OrderBook& book) {
        double base = twap_qty(p, ctx.ts);
        auto sp = book.spread_bps();
        double imb = book.imbalance(5);

        double mult = p.cfg.aggression;

        if (sp && *sp > p.cfg.max_spread_bps * 0.75) mult *= 0.5;
        if (ctx.volatility > 0.02) mult *= 0.7;
        if (ctx.toxicity > 0.7) mult *= 0.5;

        if (p.cfg.side == Side::Buy && imb < -0.25) mult *= 1.4;
        if (p.cfg.side == Side::Sell && imb > 0.25) mult *= 1.4;

        return base * std::clamp(mult, 0.1, 3.0);
    }

    static double sniper_qty(const Parent& p, const OrderBook& book) {
        auto sp = book.spread_bps();
        if (!sp || *sp > p.cfg.max_spread_bps * 0.35) return 0.0;
        return std::min(p.cfg.max_child_qty, p.remaining_qty);
    }

    static double urgency_mult(ExecUrgency u) noexcept {
        switch (u) {
            case ExecUrgency::Passive: return 0.6;
            case ExecUrgency::Normal: return 1.0;
            case ExecUrgency::Aggressive: return 1.8;
            default: return 1.0;
        }
    }

    static double choose_price(const ExecConfig& cfg, const OrderBook& book) {
        if (cfg.limit_price > 0.0) return cfg.limit_price;

        if (cfg.side == Side::Buy) {
            if (cfg.post_only) {
                auto bid = book.best_bid();
                if (bid) return *bid;

                auto ask = book.best_ask();
                return ask ? *ask : 0.0;
            }

            auto ask = book.best_ask();
            if (ask) return *ask;

            auto bid = book.best_bid();
            return bid ? *bid : 0.0;
        }

        if (cfg.side == Side::Sell) {
            if (cfg.post_only) {
                auto ask = book.best_ask();
                if (ask) return *ask;

                auto bid = book.best_bid();
                return bid ? *bid : 0.0;
            }

            auto bid = book.best_bid();
            if (bid) return *bid;

            auto ask = book.best_ask();
            return ask ? *ask : 0.0;
        }

        return 0.0;
    }
};

} // namespace hft