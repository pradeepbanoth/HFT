#pragma once

#include "adaptive_smart_order_router.hpp"
#include "child_order_adapter_handoff.hpp"
#include "child_order_commands.hpp"
#include "child_order_manager.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft::execution {

enum class SimulatedExecutionMode : uint8_t {
    Auto,
    FullFill,
    PartialFill,
    Reject
};

enum class SimulatedChildOutcome : uint8_t {
    AckOnly,
    FullFill,
    PartialFill,
    Reject,
    HandoffRejected
};

struct VenueSimulationProfile {
    double fill_probability{1.0};
    double reject_probability{0.0};

    double min_fill_ratio{1.0};
    double max_fill_ratio{1.0};

    double base_slippage_bps{0.0};
    double impact_bps_per_unit{0.0};

    uint64_t ack_latency_ns{1'000};
    uint64_t fill_latency_ns{2'000};

    bool enabled{true};
};

struct ExecutionSimulationConfig {
    SimulatedExecutionMode mode{SimulatedExecutionMode::Auto};

    uint64_t seed{42};
    uint64_t default_ack_latency_ns{1'000};
    uint64_t default_fill_latency_ns{2'000};

    double default_fill_probability{1.0};
    double default_reject_probability{0.0};

    double default_min_fill_ratio{1.0};
    double default_max_fill_ratio{1.0};

    double default_slippage_bps{0.0};
    double default_impact_bps_per_unit{0.0};

    bool generate_ack_before_reject{true};
};

struct SimulatedChildReport {
    uint64_t child_id{0};
    std::string venue;

    SimulatedChildOutcome outcome{SimulatedChildOutcome::AckOnly};

    double requested_qty{0.0};
    double filled_qty{0.0};
    double requested_price{0.0};
    double executed_price{0.0};

    uint64_t ack_ts_ns{0};
    uint64_t final_ts_ns{0};

    std::string reason;
};

struct ExecutionSimulationResult {
    bool accepted{false};

    uint64_t parent_id{0};
    hft::routing::RouteDecision route_decision{hft::routing::RouteDecision::Reject};

    std::vector<uint64_t> child_ids;
    std::vector<ChildOrderCommand> commands;
    std::vector<AdapterHandoffResult> handoff_results;
    std::vector<SimulatedChildReport> child_reports;

    double requested_qty{0.0};
    double filled_qty{0.0};
    double remaining_qty{0.0};
    double avg_fill_price{0.0};

    std::string reason;
};

struct ExecutionSimulatorMetrics {
    std::atomic<uint64_t> simulations{0};
    std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> rejected{0};

    std::atomic<uint64_t> children_simulated{0};
    std::atomic<uint64_t> full_fills{0};
    std::atomic<uint64_t> partial_fills{0};
    std::atomic<uint64_t> rejects{0};
    std::atomic<uint64_t> handoff_rejects{0};

    std::atomic<uint64_t> reports_generated{0};
};

class ExecutionSimulator {
public:
    ExecutionSimulator(
        hft::routing::AdaptiveSmartOrderRouter& router,
        ChildOrderManager& manager,
        ChildOrderCommandFactory& command_factory,
        ChildOrderAdapterHandoff& handoff,
        ExecutionSimulationConfig config = {}
    )
        : router_(router),
          manager_(manager),
          command_factory_(command_factory),
          handoff_(handoff),
          config_(config),
          rng_(config.seed) {}

    void set_venue_profile(std::string venue, VenueSimulationProfile profile) {
        venue_profiles_[std::move(venue)] = profile;
    }

    ExecutionSimulationResult run(
        const hft::routing::RouterOrder& order,
        uint64_t timestamp_ns
    ) {
        metrics_.simulations.fetch_add(1, std::memory_order_relaxed);

        ExecutionSimulationResult result;
        result.requested_qty = order.qty;

        auto route = router_.route(order);
        result.route_decision = route.decision;

        if (route.decision == hft::routing::RouteDecision::Reject) {
            result.reason = route.reason;
            metrics_.rejected.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        ParentCreateRequest parent_request;
        parent_request.symbol = order.symbol;
        parent_request.side = convert_side(order.side);
        parent_request.quantity = order.qty;
        parent_request.timestamp_ns = timestamp_ns;

        const auto parent_id = manager_.create_parent(parent_request);
        if (parent_id == 0) {
            result.reason = "Failed to create parent order";
            metrics_.rejected.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        result.parent_id = parent_id;

        std::vector<ChildOrderRoute> routes;
        routes.reserve(route.children.size());

        for (const auto& child : route.children) {
            routes.push_back({child.venue, child.qty, child.price});
        }

        auto child_ids = manager_.create_children(parent_id, routes, timestamp_ns + 1);
        if (child_ids.empty()) {
            result.reason = "Failed to create child orders";
            metrics_.rejected.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        result.child_ids = child_ids;

        auto commands = command_factory_.build_submit_commands(child_ids, timestamp_ns + 2);
        if (commands.empty()) {
            result.reason = "Failed to create submit commands";
            metrics_.rejected.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        result.commands = commands;

        auto handoff_results = handoff_.handoff_batch(commands);
        result.handoff_results = handoff_results;

        simulate_commands(commands, handoff_results, timestamp_ns, result);

        finalize_result(result);

        if (result.filled_qty > 0.0) {
            result.accepted = true;
            result.reason = "Simulation completed with execution";
            metrics_.accepted.fetch_add(1, std::memory_order_relaxed);
        } else {
            result.accepted = false;
            result.reason = "Simulation completed with no fills";
            metrics_.rejected.fetch_add(1, std::memory_order_relaxed);
        }

        return result;
    }

    const ExecutionSimulatorMetrics& metrics() const noexcept {
        return metrics_;
    }

private:
    static ChildOrderSide convert_side(hft::routing::Side side) noexcept {
        return side == hft::routing::Side::Buy
            ? ChildOrderSide::Buy
            : ChildOrderSide::Sell;
    }

    VenueSimulationProfile profile_for(const std::string& venue) const {
        auto it = venue_profiles_.find(venue);
        if (it != venue_profiles_.end()) return it->second;

        VenueSimulationProfile profile;
        profile.fill_probability = config_.default_fill_probability;
        profile.reject_probability = config_.default_reject_probability;
        profile.min_fill_ratio = config_.default_min_fill_ratio;
        profile.max_fill_ratio = config_.default_max_fill_ratio;
        profile.base_slippage_bps = config_.default_slippage_bps;
        profile.impact_bps_per_unit = config_.default_impact_bps_per_unit;
        profile.ack_latency_ns = config_.default_ack_latency_ns;
        profile.fill_latency_ns = config_.default_fill_latency_ns;
        return profile;
    }

    double uniform01() {
        return std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
    }

    double uniform(double lo, double hi) {
        if (hi < lo) std::swap(lo, hi);
        return std::uniform_real_distribution<double>(lo, hi)(rng_);
    }

    void simulate_commands(
        const std::vector<ChildOrderCommand>& commands,
        const std::vector<AdapterHandoffResult>& handoff_results,
        uint64_t base_ts_ns,
        ExecutionSimulationResult& result
    ) {
        for (std::size_t i = 0; i < commands.size() && i < handoff_results.size(); ++i) {
            const auto& command = commands[i];
            const auto& handoff_result = handoff_results[i];

            metrics_.children_simulated.fetch_add(1, std::memory_order_relaxed);

            if (handoff_result.status != AdapterHandoffStatus::Accepted) {
                SimulatedChildReport child_report;
                child_report.child_id = command.child_id;
                child_report.venue = command.venue;
                child_report.outcome = SimulatedChildOutcome::HandoffRejected;
                child_report.requested_qty = command.quantity;
                child_report.requested_price = command.price;
                child_report.reason = handoff_result.reason;

                result.child_reports.push_back(std::move(child_report));
                metrics_.handoff_rejects.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            auto profile = profile_for(command.venue);

            if (!profile.enabled) {
                generate_reject(command, base_ts_ns, "Venue disabled in simulation", profile, result);
                continue;
            }

            if (config_.mode == SimulatedExecutionMode::Reject) {
                generate_reject(command, base_ts_ns, "Forced simulated reject", profile, result);
                continue;
            }

            generate_ack(command, base_ts_ns, profile);

            if (config_.mode == SimulatedExecutionMode::FullFill) {
                generate_fill(command, command.quantity, base_ts_ns, profile, result, true);
                continue;
            }

            if (config_.mode == SimulatedExecutionMode::PartialFill) {
                const double ratio = std::clamp(profile.min_fill_ratio, 0.0, 1.0);
                generate_fill(command, command.quantity * ratio, base_ts_ns, profile, result, false);
                continue;
            }

            const double reject_roll = uniform01();

            if (reject_roll < profile.reject_probability) {
                generate_reject(command, base_ts_ns, "Probabilistic simulated reject", profile, result);
                continue;
            }

            const double fill_roll = uniform01();

            if (fill_roll <= profile.fill_probability) {
                const double ratio = std::clamp(
                    uniform(profile.min_fill_ratio, profile.max_fill_ratio),
                    0.0,
                    1.0
                );

                const double fill_qty = command.quantity * ratio;
                const bool full = std::abs(fill_qty - command.quantity) < 1e-9;

                generate_fill(command, fill_qty, base_ts_ns, profile, result, full);
            } else {
                SimulatedChildReport child_report;
                child_report.child_id = command.child_id;
                child_report.venue = command.venue;
                child_report.outcome = SimulatedChildOutcome::AckOnly;
                child_report.requested_qty = command.quantity;
                child_report.requested_price = command.price;
                child_report.ack_ts_ns = base_ts_ns + profile.ack_latency_ns;
                child_report.reason = "ACK only, no fill";

                result.child_reports.push_back(std::move(child_report));
            }
        }
    }

    void generate_ack(
        const ChildOrderCommand& command,
        uint64_t base_ts_ns,
        const VenueSimulationProfile& profile
    ) {
        ExecutionReport ack;
        ack.type = ExecutionReportType::Ack;
        ack.child_id = command.child_id;
        ack.client_order_id = command.client_order_id;
        ack.venue_order_id = command.venue + "-SIM-" + std::to_string(command.child_id);
        ack.timestamp_ns = base_ts_ns + profile.ack_latency_ns;

        manager_.on_execution_report(ack);
        metrics_.reports_generated.fetch_add(1, std::memory_order_relaxed);
    }

    void generate_reject(
        const ChildOrderCommand& command,
        uint64_t base_ts_ns,
        const std::string& reason,
        const VenueSimulationProfile& profile,
        ExecutionSimulationResult& result
    ) {
        if (config_.generate_ack_before_reject) {
            generate_ack(command, base_ts_ns, profile);
        }

        ExecutionReport reject;
        reject.type = ExecutionReportType::Reject;
        reject.child_id = command.child_id;
        reject.client_order_id = command.client_order_id;
        reject.venue_order_id = command.venue + "-SIM-" + std::to_string(command.child_id);
        reject.reason = reason;
        reject.timestamp_ns =
            base_ts_ns + profile.ack_latency_ns + profile.fill_latency_ns;

        manager_.on_execution_report(reject);
        metrics_.reports_generated.fetch_add(1, std::memory_order_relaxed);

        SimulatedChildReport child_report;
        child_report.child_id = command.child_id;
        child_report.venue = command.venue;
        child_report.outcome = SimulatedChildOutcome::Reject;
        child_report.requested_qty = command.quantity;
        child_report.requested_price = command.price;
        child_report.ack_ts_ns = base_ts_ns + profile.ack_latency_ns;
        child_report.final_ts_ns = reject.timestamp_ns;
        child_report.reason = reason;

        result.child_reports.push_back(std::move(child_report));
        metrics_.rejects.fetch_add(1, std::memory_order_relaxed);
    }

    double apply_slippage(
        const ChildOrderCommand& command,
        double fill_qty,
        const VenueSimulationProfile& profile
    ) const {
        const double total_bps =
            profile.base_slippage_bps +
            (profile.impact_bps_per_unit * fill_qty);

        const double multiplier = total_bps / 10000.0;

        if (command.side == ChildOrderSide::Buy) {
            return command.price * (1.0 + multiplier);
        }

        return command.price * (1.0 - multiplier);
    }

    void generate_fill(
        const ChildOrderCommand& command,
        double fill_qty,
        uint64_t base_ts_ns,
        const VenueSimulationProfile& profile,
        ExecutionSimulationResult& result,
        bool full
    ) {
        fill_qty = std::clamp(fill_qty, 0.0, command.quantity);

        if (fill_qty <= 0.0) {
            return;
        }

        const double exec_price = apply_slippage(command, fill_qty, profile);

        ExecutionReport fill;
        fill.type = full ? ExecutionReportType::Fill : ExecutionReportType::PartialFill;
        fill.child_id = command.child_id;
        fill.client_order_id = command.client_order_id;
        fill.venue_order_id = command.venue + "-SIM-" + std::to_string(command.child_id);
        fill.fill_qty = fill_qty;
        fill.fill_price = exec_price;
        fill.timestamp_ns =
            base_ts_ns + profile.ack_latency_ns + profile.fill_latency_ns;

        manager_.on_execution_report(fill);
        metrics_.reports_generated.fetch_add(1, std::memory_order_relaxed);

        SimulatedChildReport child_report;
        child_report.child_id = command.child_id;
        child_report.venue = command.venue;
        child_report.outcome =
            full ? SimulatedChildOutcome::FullFill : SimulatedChildOutcome::PartialFill;
        child_report.requested_qty = command.quantity;
        child_report.filled_qty = fill_qty;
        child_report.requested_price = command.price;
        child_report.executed_price = exec_price;
        child_report.ack_ts_ns = base_ts_ns + profile.ack_latency_ns;
        child_report.final_ts_ns = fill.timestamp_ns;

        result.child_reports.push_back(std::move(child_report));

        if (full) {
            metrics_.full_fills.fetch_add(1, std::memory_order_relaxed);
        } else {
            metrics_.partial_fills.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void finalize_result(ExecutionSimulationResult& result) {
        auto parent = manager_.parent(result.parent_id);

        if (!parent.has_value()) {
            return;
        }

        result.filled_qty = parent->filled_quantity;
        result.remaining_qty = parent->remaining_quantity;
        result.avg_fill_price = parent->average_price;
    }

private:
    hft::routing::AdaptiveSmartOrderRouter& router_;
    ChildOrderManager& manager_;
    ChildOrderCommandFactory& command_factory_;
    ChildOrderAdapterHandoff& handoff_;
    ExecutionSimulationConfig config_;

    std::mt19937_64 rng_;
    std::unordered_map<std::string, VenueSimulationProfile> venue_profiles_;

    ExecutionSimulatorMetrics metrics_{};
};

} // namespace hft::execution