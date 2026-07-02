#pragma once

#include "child_order_commands.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hft::execution {

enum class AdapterHandoffStatus : uint8_t {
    Accepted,
    Rejected,
    AdapterMissing,
    AdapterDisabled,
    InvalidCommand,
    ExceptionThrown
};

struct AdapterHandoffResult {
    AdapterHandoffStatus status{AdapterHandoffStatus::Rejected};
    uint64_t child_id{0};
    uint64_t parent_id{0};
    std::string venue;
    std::string reason;
    uint64_t start_ns{0};
    uint64_t end_ns{0};
};

struct AdapterVenueState {
    bool enabled{true};
    uint64_t accepted{0};
    uint64_t rejected{0};
    uint64_t last_handoff_ns{0};
};

struct AdapterHandoffMetrics {
    std::atomic<uint64_t> submit_attempts{0};
    std::atomic<uint64_t> cancel_attempts{0};
    std::atomic<uint64_t> replace_attempts{0};
    std::atomic<uint64_t> retry_attempts{0};

    std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<uint64_t> missing_adapter{0};
    std::atomic<uint64_t> disabled_adapter{0};
    std::atomic<uint64_t> invalid_command{0};
    std::atomic<uint64_t> exceptions{0};

    std::atomic<uint64_t> latency_count{0};
    std::atomic<uint64_t> latency_total_ns{0};
    std::atomic<uint64_t> latency_max_ns{0};
};

struct AdapterHandoffMetricsSnapshot {
    uint64_t submit_attempts{0};
    uint64_t cancel_attempts{0};
    uint64_t replace_attempts{0};
    uint64_t retry_attempts{0};

    uint64_t accepted{0};
    uint64_t rejected{0};
    uint64_t missing_adapter{0};
    uint64_t disabled_adapter{0};
    uint64_t invalid_command{0};
    uint64_t exceptions{0};

    double avg_latency_us{0.0};
    double max_latency_us{0.0};
};

class IChildOrderExecutionAdapter {
public:
    virtual ~IChildOrderExecutionAdapter() = default;

    virtual bool submit_child_order(
        const ChildOrderCommand& command,
        std::string& reject_reason
    ) = 0;

    virtual bool cancel_child_order(
        const ChildOrderCommand& command,
        std::string& reject_reason
    ) = 0;

    virtual bool replace_child_order(
        const ChildOrderCommand& command,
        std::string& reject_reason
    ) = 0;
};

class ChildOrderAdapterHandoff {
public:
    using ResultCallback = std::function<void(const AdapterHandoffResult&)>;

    void register_adapter(
        std::string venue,
        std::shared_ptr<IChildOrderExecutionAdapter> adapter
    ) {
        if (venue.empty() || !adapter) return;

        adapters_[venue] = std::move(adapter);

        if (!venue_state_.contains(venue)) {
            venue_state_[venue] = AdapterVenueState{};
        }
    }

    bool unregister_adapter(const std::string& venue) {
        venue_state_.erase(venue);
        return adapters_.erase(venue) > 0;
    }

    bool has_adapter(const std::string& venue) const {
        return adapters_.find(venue) != adapters_.end();
    }

    bool set_venue_enabled(const std::string& venue, bool enabled) {
        auto it = venue_state_.find(venue);
        if (it == venue_state_.end()) return false;

        it->second.enabled = enabled;
        return true;
    }

    bool venue_enabled(const std::string& venue) const {
        auto it = venue_state_.find(venue);
        if (it == venue_state_.end()) return false;

        return it->second.enabled;
    }

    std::optional<AdapterVenueState> venue_state(const std::string& venue) const {
        auto it = venue_state_.find(venue);
        if (it == venue_state_.end()) return std::nullopt;

        return it->second;
    }

    void set_result_callback(ResultCallback callback) {
        callback_ = std::move(callback);
    }

    AdapterHandoffResult handoff(const ChildOrderCommand& command) {
        count_attempt(command.type);

        const auto start = command.timestamp_ns;

        auto invalid = validate(command);
        if (invalid.has_value()) {
            metrics_.invalid_command.fetch_add(1, std::memory_order_relaxed);
            metrics_.rejected.fetch_add(1, std::memory_order_relaxed);

            auto result = make_result(
                AdapterHandoffStatus::InvalidCommand,
                command,
                *invalid,
                start,
                start
            );

            emit(result);
            return result;
        }

        auto state_it = venue_state_.find(command.venue);

        if (state_it != venue_state_.end() && !state_it->second.enabled) {
            metrics_.disabled_adapter.fetch_add(1, std::memory_order_relaxed);
            metrics_.rejected.fetch_add(1, std::memory_order_relaxed);

            auto result = make_result(
                AdapterHandoffStatus::AdapterDisabled,
                command,
                "Venue adapter disabled",
                start,
                start
            );

            state_it->second.rejected += 1;
            emit(result);
            return result;
        }

        auto adapter_it = adapters_.find(command.venue);

        if (adapter_it == adapters_.end()) {
            metrics_.missing_adapter.fetch_add(1, std::memory_order_relaxed);
            metrics_.rejected.fetch_add(1, std::memory_order_relaxed);

            auto result = make_result(
                AdapterHandoffStatus::AdapterMissing,
                command,
                "No adapter registered for venue",
                start,
                start
            );

            emit(result);
            return result;
        }

        std::string reason;
        bool ok = false;

        try {
            switch (command.type) {
                case ChildCommandType::Submit:
                case ChildCommandType::Retry:
                    ok = adapter_it->second->submit_child_order(command, reason);
                    break;

                case ChildCommandType::Cancel:
                    ok = adapter_it->second->cancel_child_order(command, reason);
                    break;

                case ChildCommandType::Replace:
                    ok = adapter_it->second->replace_child_order(command, reason);
                    break;
            }
        } catch (...) {
            metrics_.exceptions.fetch_add(1, std::memory_order_relaxed);
            metrics_.rejected.fetch_add(1, std::memory_order_relaxed);

            auto result = make_result(
                AdapterHandoffStatus::ExceptionThrown,
                command,
                "Adapter threw exception",
                start,
                start
            );

            mark_venue_reject(command.venue, start);
            emit(result);
            return result;
        }

        const auto end = start == 0 ? 0 : start + 1;

        if (!ok) {
            metrics_.rejected.fetch_add(1, std::memory_order_relaxed);

            auto result = make_result(
                AdapterHandoffStatus::Rejected,
                command,
                reason.empty() ? "Adapter rejected command" : reason,
                start,
                end
            );

            mark_venue_reject(command.venue, end);
            observe_latency(start, end);
            emit(result);
            return result;
        }

        metrics_.accepted.fetch_add(1, std::memory_order_relaxed);

        auto result = make_result(
            AdapterHandoffStatus::Accepted,
            command,
            {},
            start,
            end
        );

        mark_venue_accept(command.venue, end);
        observe_latency(start, end);
        emit(result);
        return result;
    }

    std::vector<AdapterHandoffResult> handoff_batch(
        const std::vector<ChildOrderCommand>& commands
    ) {
        std::vector<AdapterHandoffResult> results;
        results.reserve(commands.size());

        for (const auto& command : commands) {
            results.push_back(handoff(command));
        }

        return results;
    }

    const AdapterHandoffMetrics& metrics() const noexcept {
        return metrics_;
    }

    AdapterHandoffMetricsSnapshot metrics_snapshot() const noexcept {
        AdapterHandoffMetricsSnapshot out;

        out.submit_attempts = metrics_.submit_attempts.load(std::memory_order_relaxed);
        out.cancel_attempts = metrics_.cancel_attempts.load(std::memory_order_relaxed);
        out.replace_attempts = metrics_.replace_attempts.load(std::memory_order_relaxed);
        out.retry_attempts = metrics_.retry_attempts.load(std::memory_order_relaxed);

        out.accepted = metrics_.accepted.load(std::memory_order_relaxed);
        out.rejected = metrics_.rejected.load(std::memory_order_relaxed);
        out.missing_adapter = metrics_.missing_adapter.load(std::memory_order_relaxed);
        out.disabled_adapter = metrics_.disabled_adapter.load(std::memory_order_relaxed);
        out.invalid_command = metrics_.invalid_command.load(std::memory_order_relaxed);
        out.exceptions = metrics_.exceptions.load(std::memory_order_relaxed);

        const auto count = metrics_.latency_count.load(std::memory_order_relaxed);
        const auto total = metrics_.latency_total_ns.load(std::memory_order_relaxed);
        const auto maxv = metrics_.latency_max_ns.load(std::memory_order_relaxed);

        out.avg_latency_us = count == 0 ? 0.0 : static_cast<double>(total) / count / 1000.0;
        out.max_latency_us = static_cast<double>(maxv) / 1000.0;

        return out;
    }

private:
    static AdapterHandoffResult make_result(
        AdapterHandoffStatus status,
        const ChildOrderCommand& command,
        std::string reason,
        uint64_t start_ns,
        uint64_t end_ns
    ) {
        AdapterHandoffResult result;
        result.status = status;
        result.child_id = command.child_id;
        result.parent_id = command.parent_id;
        result.venue = command.venue;
        result.reason = std::move(reason);
        result.start_ns = start_ns;
        result.end_ns = end_ns;
        return result;
    }

    void emit(const AdapterHandoffResult& result) const {
        if (callback_) {
            callback_(result);
        }
    }

    void count_attempt(ChildCommandType type) {
        switch (type) {
            case ChildCommandType::Submit:
                metrics_.submit_attempts.fetch_add(1, std::memory_order_relaxed);
                break;

            case ChildCommandType::Cancel:
                metrics_.cancel_attempts.fetch_add(1, std::memory_order_relaxed);
                break;

            case ChildCommandType::Replace:
                metrics_.replace_attempts.fetch_add(1, std::memory_order_relaxed);
                break;

            case ChildCommandType::Retry:
                metrics_.retry_attempts.fetch_add(1, std::memory_order_relaxed);
                break;
        }
    }

    static std::optional<std::string> validate(const ChildOrderCommand& command) {
        if (command.child_id == 0) return "Invalid child id";
        if (command.parent_id == 0) return "Invalid parent id";
        if (command.venue.empty()) return "Missing venue";
        if (command.symbol.empty()) return "Missing symbol";
        if (command.client_order_id.empty()) return "Missing client order id";

        switch (command.type) {
            case ChildCommandType::Submit:
            case ChildCommandType::Retry:
                if (command.quantity <= 0.0) return "Invalid submit quantity";
                if (command.price <= 0.0) return "Invalid submit price";
                break;

            case ChildCommandType::Cancel:
                if (command.venue_order_id.empty() && command.client_order_id.empty()) {
                    return "Missing cancel order id";
                }
                break;

            case ChildCommandType::Replace:
                if (command.quantity <= 0.0) return "Invalid replace quantity";
                if (command.price <= 0.0) return "Invalid replace price";
                if (command.venue_order_id.empty() && command.client_order_id.empty()) {
                    return "Missing replace order id";
                }
                break;
        }

        return std::nullopt;
    }

    void mark_venue_accept(const std::string& venue, uint64_t now_ns) {
        auto& state = venue_state_[venue];
        state.accepted += 1;
        state.last_handoff_ns = now_ns;
    }

    void mark_venue_reject(const std::string& venue, uint64_t now_ns) {
        auto& state = venue_state_[venue];
        state.rejected += 1;
        state.last_handoff_ns = now_ns;
    }

    void observe_latency(uint64_t start_ns, uint64_t end_ns) {
        if (start_ns == 0 || end_ns < start_ns) return;

        const auto latency = end_ns - start_ns;

        metrics_.latency_count.fetch_add(1, std::memory_order_relaxed);
        metrics_.latency_total_ns.fetch_add(latency, std::memory_order_relaxed);

        auto current = metrics_.latency_max_ns.load(std::memory_order_relaxed);

        while (
            latency > current &&
            !metrics_.latency_max_ns.compare_exchange_weak(
                current,
                latency,
                std::memory_order_relaxed,
                std::memory_order_relaxed
            )
        ) {}
    }

private:
    std::unordered_map<std::string, std::shared_ptr<IChildOrderExecutionAdapter>> adapters_;
    std::unordered_map<std::string, AdapterVenueState> venue_state_;

    ResultCallback callback_;
    AdapterHandoffMetrics metrics_{};
};

class MockChildOrderExecutionAdapter final : public IChildOrderExecutionAdapter {
public:
    bool accept_submit{true};
    bool accept_cancel{true};
    bool accept_replace{true};
    bool throw_on_submit{false};
    bool throw_on_cancel{false};
    bool throw_on_replace{false};

    std::vector<ChildOrderCommand> submitted;
    std::vector<ChildOrderCommand> cancelled;
    std::vector<ChildOrderCommand> replaced;

    bool submit_child_order(
        const ChildOrderCommand& command,
        std::string& reject_reason
    ) override {
        if (throw_on_submit) {
            throw 1;
        }

        if (!accept_submit) {
            reject_reason = "Mock submit reject";
            return false;
        }

        submitted.push_back(command);
        return true;
    }

    bool cancel_child_order(
        const ChildOrderCommand& command,
        std::string& reject_reason
    ) override {
        if (throw_on_cancel) {
            throw 1;
        }

        if (!accept_cancel) {
            reject_reason = "Mock cancel reject";
            return false;
        }

        cancelled.push_back(command);
        return true;
    }

    bool replace_child_order(
        const ChildOrderCommand& command,
        std::string& reject_reason
    ) override {
        if (throw_on_replace) {
            throw 1;
        }

        if (!accept_replace) {
            reject_reason = "Mock replace reject";
            return false;
        }

        replaced.push_back(command);
        return true;
    }
};

} // namespace hft::execution