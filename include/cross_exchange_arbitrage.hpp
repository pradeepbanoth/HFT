#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft::arbitrage {

inline uint64_t arb_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

enum class ArbLegSide : uint8_t { Buy, Sell };
enum class ArbState : uint8_t { Rejected, Detected, Executable };
enum class ArbExecutionMode : uint8_t { Simultaneous, BuyFirst, SellFirst };

struct VenueMarket {
    std::string venue;
    std::string symbol;

    double bid{0.0};
    double ask{0.0};
    double bid_qty{0.0};
    double ask_qty{0.0};

    double taker_fee_bps{0.0};
    double maker_fee_bps{0.0};
    double latency_us{0.0};

    uint64_t update_ts_ns{0};
    bool healthy{true};
};

struct ArbitrageConfig {
    double min_net_edge_bps{4.0};
    double safety_slippage_bps{1.5};
    double latency_penalty_bps_per_ms{0.15};

    double min_qty{0.000001};
    double max_qty{1000000.0};

    double min_notional{10.0};
    double max_notional{250000.0};

    uint64_t max_quote_age_ns{2'000'000'000ULL};
    uint64_t cooldown_ns{100'000'000ULL};

    ArbExecutionMode execution_mode{ArbExecutionMode::Simultaneous};
};

struct ArbLeg {
    std::string venue;
    std::string symbol;
    ArbLegSide side{ArbLegSide::Buy};

    double quantity{0.0};
    double price{0.0};
    double fee_bps{0.0};
    double notional{0.0};
};

struct ArbitrageOpportunity {
    ArbState state{ArbState::Rejected};

    std::string symbol;
    std::string buy_venue;
    std::string sell_venue;

    double buy_price{0.0};
    double sell_price{0.0};
    double quantity{0.0};

    double gross_edge_bps{0.0};
    double net_edge_bps{0.0};
    double expected_profit{0.0};
    double confidence{0.0};

    uint64_t detected_ts_ns{0};
    std::string reason;
};

struct ArbitrageExecutionPlan {
    bool executable{false};
    ArbitrageOpportunity opportunity{};
    ArbLeg buy_leg{};
    ArbLeg sell_leg{};
    ArbExecutionMode mode{ArbExecutionMode::Simultaneous};
};

struct ArbitrageMetrics {
    std::atomic<uint64_t> scans{0};
    std::atomic<uint64_t> markets_updated{0};
    std::atomic<uint64_t> opportunities_detected{0};
    std::atomic<uint64_t> opportunities_executable{0};
    std::atomic<uint64_t> rejected_no_spread{0};
    std::atomic<uint64_t> rejected_low_edge{0};
    std::atomic<uint64_t> rejected_stale{0};
    std::atomic<uint64_t> rejected_notional{0};
    std::atomic<uint64_t> rejected_cooldown{0};
};

class CrossExchangeArbitrageEngine {
public:
    explicit CrossExchangeArbitrageEngine(ArbitrageConfig config = {})
        : config_(config) {}

    void update_market(VenueMarket market) {
        if (market.update_ts_ns == 0) {
            market.update_ts_ns = arb_now_ns();
        }

        markets_[key(market.venue, market.symbol)] = std::move(market);
        metrics_.markets_updated.fetch_add(1, std::memory_order_relaxed);
    }

    std::optional<ArbitrageOpportunity> best_opportunity(const std::string& symbol) {
    if (in_cooldown(symbol)) {
        metrics_.rejected_cooldown.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    auto opportunities = scan_all(symbol);
    if (opportunities.empty()) return std::nullopt;
    return opportunities.front();
    }

    std::vector<ArbitrageOpportunity> scan_all(const std::string& symbol) {
    metrics_.scans.fetch_add(1, std::memory_order_relaxed);

    if (in_cooldown(symbol)) {
        metrics_.rejected_cooldown.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    std::vector<ArbitrageOpportunity> out;
        auto venues = collect(symbol);

        for (const auto& buy : venues) {
            for (const auto& sell : venues) {
                if (buy.venue == sell.venue) continue;

                auto opp = evaluate_pair(buy, sell);
                if (opp.state == ArbState::Executable) {
                    out.push_back(std::move(opp));
                }
            }
        }

        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            if (a.net_edge_bps != b.net_edge_bps) return a.net_edge_bps > b.net_edge_bps;
            return a.confidence > b.confidence;
        });

        metrics_.opportunities_executable.fetch_add(out.size(), std::memory_order_relaxed);
        return out;
    }

    std::optional<ArbitrageExecutionPlan> build_execution_plan(const std::string& symbol) {
    if (in_cooldown(symbol)) {
        metrics_.rejected_cooldown.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    auto opportunities = scan_all(symbol);
    if (opportunities.empty()) return std::nullopt;

    const auto& best = opportunities.front();

    ArbitrageExecutionPlan plan;
    plan.executable = true;
    plan.opportunity = best;
    plan.mode = config_.execution_mode;

    plan.buy_leg = ArbLeg{
        best.buy_venue,
        best.symbol,
        ArbLegSide::Buy,
        best.quantity,
        best.buy_price,
        0.0,
        best.quantity * best.buy_price
    };

    plan.sell_leg = ArbLeg{
        best.sell_venue,
        best.symbol,
        ArbLegSide::Sell,
        best.quantity,
        best.sell_price,
        0.0,
        best.quantity * best.sell_price
    };

    last_execution_ns_[symbol] = arb_now_ns();

    return plan;
}

    const ArbitrageMetrics& metrics() const noexcept {
        return metrics_;
    }

private:
    static std::string key(const std::string& venue, const std::string& symbol) {
        return venue + ":" + symbol;
    }

    bool in_cooldown(const std::string& symbol) const {
        auto it = last_execution_ns_.find(symbol);
        if (it == last_execution_ns_.end()) return false;
        return arb_now_ns() - it->second < config_.cooldown_ns;
    }

    bool stale(const VenueMarket& m) const {
        return arb_now_ns() - m.update_ts_ns > config_.max_quote_age_ns;
    }

    std::vector<VenueMarket> collect(const std::string& symbol) const {
        std::vector<VenueMarket> out;

        for (const auto& [_, m] : markets_) {
            if (!m.healthy) continue;
            if (m.symbol != symbol) continue;
            if (m.bid <= 0.0 || m.ask <= 0.0) continue;
            if (m.bid_qty <= 0.0 || m.ask_qty <= 0.0) continue;
            if (stale(m)) continue;

            out.push_back(m);
        }

        return out;
    }

    ArbitrageOpportunity evaluate_pair(const VenueMarket& buy, const VenueMarket& sell) {
        ArbitrageOpportunity opp;
        opp.symbol = buy.symbol;
        opp.buy_venue = buy.venue;
        opp.sell_venue = sell.venue;
        opp.buy_price = buy.ask;
        opp.sell_price = sell.bid;
        opp.detected_ts_ns = arb_now_ns();

        if (in_cooldown(buy.symbol)) {
            opp.reason = "Symbol in cooldown";
            metrics_.rejected_cooldown.fetch_add(1, std::memory_order_relaxed);
            return opp;
        }

        if (stale(buy) || stale(sell)) {
            opp.reason = "Stale quote";
            metrics_.rejected_stale.fetch_add(1, std::memory_order_relaxed);
            return opp;
        }

        if (sell.bid <= buy.ask) {
            opp.reason = "No positive spread";
            metrics_.rejected_no_spread.fetch_add(1, std::memory_order_relaxed);
            return opp;
        }

        const double mid = (buy.ask + sell.bid) * 0.5;
        const double gross_edge_bps = ((sell.bid - buy.ask) / mid) * 10000.0;
        const double fee_bps = buy.taker_fee_bps + sell.taker_fee_bps;
        const double latency_penalty_bps =
            ((buy.latency_us + sell.latency_us) / 1000.0) *
            config_.latency_penalty_bps_per_ms;

        const double net_edge_bps =
            gross_edge_bps -
            fee_bps -
            config_.safety_slippage_bps -
            latency_penalty_bps;

        opp.gross_edge_bps = gross_edge_bps;
        opp.net_edge_bps = net_edge_bps;

        metrics_.opportunities_detected.fetch_add(1, std::memory_order_relaxed);

        if (net_edge_bps < config_.min_net_edge_bps) {
            opp.reason = "Net edge below threshold";
            metrics_.rejected_low_edge.fetch_add(1, std::memory_order_relaxed);
            return opp;
        }

        const double qty = std::min({
            buy.ask_qty,
            sell.bid_qty,
            config_.max_qty
        });

        const double notional = qty * buy.ask;

        if (qty < config_.min_qty ||
            notional < config_.min_notional ||
            notional > config_.max_notional) {
            opp.reason = "Notional or quantity constraints failed";
            metrics_.rejected_notional.fetch_add(1, std::memory_order_relaxed);
            return opp;
        }

        opp.quantity = qty;
        opp.expected_profit = (sell.bid - buy.ask) * qty;
        opp.confidence = confidence_score(net_edge_bps, buy, sell);
        opp.state = ArbState::Executable;
        opp.reason = "Executable arbitrage";

        return opp;
    }

    double confidence_score(
        double net_edge_bps,
        const VenueMarket& buy,
        const VenueMarket& sell
    ) const {
        const double edge_score = std::min(1.0, net_edge_bps / 25.0);
        const double latency_score =
            1.0 - std::min(1.0, (buy.latency_us + sell.latency_us) / 10000.0);

        const double liquidity =
            std::min(buy.ask_qty, sell.bid_qty);

        const double liquidity_score = std::min(1.0, liquidity / config_.max_qty);

        return std::clamp(
            0.55 * edge_score + 0.25 * latency_score + 0.20 * liquidity_score,
            0.0,
            1.0
        );
    }

private:
    ArbitrageConfig config_;
    std::unordered_map<std::string, VenueMarket> markets_;
    std::unordered_map<std::string, uint64_t> last_execution_ns_;
    ArbitrageMetrics metrics_{};
};

} // namespace hft::arbitrage